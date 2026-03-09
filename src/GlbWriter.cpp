#include "GlbWriter.h"
#include "Logger.h"

// tinygltf - header-only（在一个 .cpp 中实现）
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <cstring>
#include <cassert>

#ifdef HAVE_DRACO
#  include <draco/compression/encode.h>
#  include <draco/mesh/mesh.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// B3DM 魔数
static const char B3DM_MAGIC[4] = {'b','3','d','m'};
static const uint32_t B3DM_VERSION = 1;

#ifdef HAVE_DRACO
struct DracoResult { std::vector<uint8_t> bytes; int posAttrId=-1, uvAttrId=-1; };

static DracoResult encodeDraco(
    const std::vector<float>&    verts,
    const std::vector<float>&    uvs,
    const std::vector<uint32_t>& indices,
    int quantBits)
{
    DracoResult res;
    size_t vtxN = verts.size()/3, triN = indices.size()/3;
    draco::Mesh mesh;
    mesh.SetNumFaces(static_cast<unsigned>(triN));
    mesh.set_num_points(static_cast<unsigned>(vtxN));
    // POSITION
    { draco::GeometryAttribute a;
      a.Init(draco::GeometryAttribute::POSITION, nullptr, 3, draco::DT_FLOAT32, false, 12, 0);
      res.posAttrId = mesh.AddAttribute(a, true, static_cast<unsigned>(vtxN)); }
    // TEX_COORD
    if (!uvs.empty()) {
        draco::GeometryAttribute a;
        a.Init(draco::GeometryAttribute::TEX_COORD, nullptr, 2, draco::DT_FLOAT32, false, 8, 0);
        res.uvAttrId = mesh.AddAttribute(a, true, static_cast<unsigned>(vtxN));
    }
    // Vertex data
    for (size_t i = 0; i < vtxN; ++i) {
        mesh.attribute(res.posAttrId)->SetAttributeValue(draco::AttributeValueIndex(static_cast<unsigned>(i)), &verts[i*3]);
        if (res.uvAttrId>=0) mesh.attribute(res.uvAttrId)->SetAttributeValue(draco::AttributeValueIndex(static_cast<unsigned>(i)), &uvs[i*2]);
    }
    // Faces
    for (size_t i = 0; i < triN; ++i) {
        draco::Mesh::Face f;
        f[0]=draco::PointIndex(indices[i*3]); f[1]=draco::PointIndex(indices[i*3+1]); f[2]=draco::PointIndex(indices[i*3+2]);
        mesh.SetFace(draco::FaceIndex(static_cast<unsigned>(i)), f);
    }
    // Encode
    draco::Encoder enc;
    enc.SetAttributeQuantization(draco::GeometryAttribute::POSITION, quantBits);
    if (res.uvAttrId>=0) enc.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 10);
    enc.SetSpeedOptions(5, 5);
    draco::EncoderBuffer buf;
    if (!enc.EncodeMeshToBuffer(mesh, &buf).ok()) return res;
    res.bytes.assign(reinterpret_cast<const uint8_t*>(buf.data()),
                     reinterpret_cast<const uint8_t*>(buf.data())+buf.size());
    return res;
}
#endif  // HAVE_DRACO

GlbWriter::GlbWriter(const ConvertConfig& cfg) : cfg_(cfg) {}

bool GlbWriter::write(const TileNode& node,
                      const std::vector<uint8_t>& texData,
                      const std::string& mimeType,
                      const std::string& outPath)
{
    // 确保输出目录存在
    fs::create_directories(fs::path(outPath).parent_path());

    if (cfg_.tileFormat == "b3dm") {
        return writeB3dm(node, texData, mimeType, outPath);
    } else {
        return writeGlb(node, texData, mimeType, outPath);
    }
}

bool GlbWriter::writeGlb(const TileNode& node,
                          const std::vector<uint8_t>& texData,
                          const std::string& mimeType,
                          const std::string& outPath)
{
    tinygltf::Model model;
    tinygltf::Scene scene;
    tinygltf::Buffer buf;

    model.extensionsUsed.push_back("KHR_materials_unlit");
    // KTX2 Basis Universal
    if (mimeType == "image/ktx2") {
        model.extensionsUsed.push_back("KHR_texture_basisu");
        model.extensionsRequired.push_back("KHR_texture_basisu");
    }

    // ── 辅助 lambda：添加 BufferView ──────────────────────────────
    auto addBV = [&](const void* data, size_t byteLen, int target) -> int {
        while (buf.data.size() % 4 != 0) buf.data.push_back(0);  // 对齐
        size_t offset = buf.data.size();
        buf.data.resize(offset + byteLen);
        if (data && byteLen > 0)
            std::memcpy(buf.data.data() + offset, data, byteLen);
        int idx = static_cast<int>(model.bufferViews.size());
        tinygltf::BufferView bv;
        bv.buffer     = 0;
        bv.byteOffset = static_cast<int>(offset);
        bv.byteLength = static_cast<int>(byteLen);
        bv.target     = target;
        model.bufferViews.push_back(std::move(bv));
        return idx;
    };

    // ── 辅助 lambda：添加 Material ─────────────────────────────────
    auto addMaterial = [&](int texIdx) -> int {
        tinygltf::Material mat;
        mat.name = "mat" + std::to_string(model.materials.size());
        mat.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
        mat.pbrMetallicRoughness.metallicFactor  = 0.0;
        mat.pbrMetallicRoughness.roughnessFactor = 0.5;
        mat.pbrMetallicRoughness.baseColorFactor = {1.0, 1.0, 1.0, 1.0};
        if (texIdx >= 0) {
            mat.pbrMetallicRoughness.baseColorTexture.index    = texIdx;
            mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
        }
        mat.doubleSided = true;
        int idx = static_cast<int>(model.materials.size());
        model.materials.push_back(std::move(mat));
        return idx;
    };

    // ── 辅助 lambda：添加 Image+Sampler+Texture，返回 texture idx ──
    auto addTexture = [&](int bvImg, const std::string& mime) -> int {
        if (bvImg < 0) return -1;
        tinygltf::Image img;
        img.bufferView = bvImg;
        img.mimeType   = mime;
        int imgIdx = static_cast<int>(model.images.size());
        model.images.push_back(std::move(img));

        tinygltf::Sampler smp;
        smp.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        smp.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        smp.wrapS     = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
        smp.wrapT     = TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
        int smpIdx = static_cast<int>(model.samplers.size());
        model.samplers.push_back(std::move(smp));

        tinygltf::Texture tex;
        tex.sampler = smpIdx;
        if (mime == "image/ktx2") {
            // KHR_texture_basisu: KTX2 image referenced via extension.source
            tex.source = -1; // no fallback image
            tinygltf::Value::Object basisExt;
            basisExt["source"] = tinygltf::Value(imgIdx);
            tex.extensions["KHR_texture_basisu"] = tinygltf::Value(basisExt);
        } else {
            tex.source = imgIdx;
        }
        int texIdx = static_cast<int>(model.textures.size());
        model.textures.push_back(std::move(tex));
        return texIdx;
    };

    // ── Draco 压缩设置 ────────────────────────────────────────────
    bool useDraco = cfg_.compressGeometry;
#ifndef HAVE_DRACO
    if (useDraco) { LOG_WARN("Draco 未编译，ignorandcompressGeometry"); useDraco = false; }
#endif
    if (useDraco) {
        model.extensionsUsed.push_back("KHR_draco_mesh_compression");
        model.extensionsRequired.push_back("KHR_draco_mesh_compression");
    }

    // ── 决定使用 SubMesh 还是旧的合并数据 ─────────────────────────
    tinygltf::Mesh mesh;
    mesh.name = node.nodeId;

    // 构建 KHR_draco_mesh_compression extension Value 的辅助 lambda
    auto makeDracoExt = [](int bvDraco, int posId, int uvId) {
        tinygltf::Value::Object dracoAttribs;
        dracoAttribs["POSITION"] = tinygltf::Value(posId);
        if (uvId >= 0) dracoAttribs["TEXCOORD_0"] = tinygltf::Value(uvId);
        tinygltf::Value::Object dracoExt;
        dracoExt["bufferView"] = tinygltf::Value(bvDraco);
        dracoExt["attributes"] = tinygltf::Value(dracoAttribs);
        tinygltf::Value::Object ext;
        ext["KHR_draco_mesh_compression"] = tinygltf::Value(dracoExt);
        return ext;
    }; (void)makeDracoExt; // suppress unused warning if HAVE_DRACO not defined

    if (!node.subMeshes.empty()) {
        // ◆ 多 Geode 路径
        for (size_t si = 0; si < node.subMeshes.size(); ++si) {
            const SubMesh& sm = node.subMeshes[si];
            if (sm.vertices.empty() || sm.indices.empty()) continue;
            size_t vtxCount = sm.vertices.size() / 3;

            // 纹理（两路径公用）
            int bvImg = sm.encodedData.empty() ? -1
                : addBV(sm.encodedData.data(), sm.encodedData.size(), 0);
            int texIdx = (!sm.uvs.empty() && bvImg >= 0) ? addTexture(bvImg, mimeType) : -1;
            int matIdx = addMaterial(texIdx);

            // AABB（两路径公用）
            std::vector<double> posMin={1e18,1e18,1e18}, posMax={-1e18,-1e18,-1e18};
            for (size_t i=0;i<vtxCount;++i) for(int k=0;k<3;++k){
                double v=sm.vertices[i*3+k]; posMin[k]=std::min(posMin[k],v); posMax[k]=std::max(posMax[k],v);}

            tinygltf::Primitive prim;
            prim.material = matIdx; prim.mode = TINYGLTF_MODE_TRIANGLES;

#ifdef HAVE_DRACO
            if (useDraco && sm.indices.size()%3==0) {
                auto dr = encodeDraco(sm.vertices, sm.uvs, sm.indices, cfg_.dracoQuantBits);
                if (!dr.bytes.empty()) {
                    int bvD = addBV(dr.bytes.data(), dr.bytes.size(), 0);
                    int aVtx = static_cast<int>(model.accessors.size());
                    { tinygltf::Accessor a; a.bufferView=-1; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                      a.type=TINYGLTF_TYPE_VEC3; a.count=static_cast<int>(vtxCount);
                      a.minValues=posMin; a.maxValues=posMax; model.accessors.push_back(std::move(a)); }
                    prim.attributes["POSITION"]=aVtx;
                    if (!sm.uvs.empty()&&dr.uvAttrId>=0) {
                        int aUV=static_cast<int>(model.accessors.size());
                        tinygltf::Accessor a; a.bufferView=-1; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                        a.type=TINYGLTF_TYPE_VEC2; a.count=static_cast<int>(vtxCount);
                        model.accessors.push_back(std::move(a)); prim.attributes["TEXCOORD_0"]=aUV; }
                    int aIdx=static_cast<int>(model.accessors.size());
                    { tinygltf::Accessor a; a.bufferView=-1;
                      a.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                      a.type=TINYGLTF_TYPE_SCALAR; a.count=static_cast<int>(sm.indices.size());
                      model.accessors.push_back(std::move(a)); }
                    prim.indices=aIdx;
                    prim.extensions = makeDracoExt(bvD, dr.posAttrId, dr.uvAttrId);
                    mesh.primitives.push_back(std::move(prim));
                    continue;
                }
            }
#endif
            // 标准路径（无 Draco）
            {
                int bvVtx=addBV(sm.vertices.data(),sm.vertices.size()*4,TINYGLTF_TARGET_ARRAY_BUFFER);
                int bvUV=-1;
                if (!sm.uvs.empty()) bvUV=addBV(sm.uvs.data(),sm.uvs.size()*4,TINYGLTF_TARGET_ARRAY_BUFFER);
                int bvIdx=addBV(sm.indices.data(),sm.indices.size()*4,TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
                int aVtx=static_cast<int>(model.accessors.size());
                { tinygltf::Accessor a; a.bufferView=bvVtx; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                  a.type=TINYGLTF_TYPE_VEC3; a.count=static_cast<int>(vtxCount);
                  a.minValues=posMin; a.maxValues=posMax; model.accessors.push_back(std::move(a)); }
                prim.attributes["POSITION"]=aVtx;
                if (bvUV>=0) {
                    int aUV=static_cast<int>(model.accessors.size());
                    tinygltf::Accessor a; a.bufferView=bvUV; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                    a.type=TINYGLTF_TYPE_VEC2; a.count=static_cast<int>(vtxCount);
                    model.accessors.push_back(std::move(a)); prim.attributes["TEXCOORD_0"]=aUV; }
                int aIdx=static_cast<int>(model.accessors.size());
                { tinygltf::Accessor a; a.bufferView=bvIdx;
                  a.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                  a.type=TINYGLTF_TYPE_SCALAR; a.count=static_cast<int>(sm.indices.size());
                  model.accessors.push_back(std::move(a)); }
                prim.indices=aIdx;
                mesh.primitives.push_back(std::move(prim));
            }
        }
    } else {
        // ◆ 兜底路径（单纹理合并数据）
        size_t vtxCount = node.vertices.size()/3;
        std::vector<double> posMin={1e18,1e18,1e18}, posMax={-1e18,-1e18,-1e18};
        for (size_t i=0;i<vtxCount;++i) for(int k=0;k<3;++k){
            double v=node.vertices[i*3+k]; posMin[k]=std::min(posMin[k],v); posMax[k]=std::max(posMax[k],v);}
        int bvImg=texData.empty()?-1:addBV(texData.data(),texData.size(),0);
        int texIdx=(!node.uvs.empty()&&bvImg>=0)?addTexture(bvImg,mimeType):-1;
        int matIdx=addMaterial(texIdx);
        tinygltf::Primitive prim; prim.material=matIdx; prim.mode=TINYGLTF_MODE_TRIANGLES;
        bool dracoUsed=false;
#ifdef HAVE_DRACO
        if (useDraco && !node.vertices.empty() && !node.indices.empty() && node.indices.size()%3==0) {
            auto dr=encodeDraco(node.vertices,node.uvs,node.indices,cfg_.dracoQuantBits);
            if (!dr.bytes.empty()) {
                int bvD=addBV(dr.bytes.data(),dr.bytes.size(),0);
                int aVtx=static_cast<int>(model.accessors.size());
                { tinygltf::Accessor a; a.bufferView=-1; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                  a.type=TINYGLTF_TYPE_VEC3; a.count=static_cast<int>(vtxCount);
                  a.minValues=posMin; a.maxValues=posMax; model.accessors.push_back(std::move(a)); }
                prim.attributes["POSITION"]=aVtx;
                if (!node.uvs.empty()&&dr.uvAttrId>=0) {
                    int aUV=static_cast<int>(model.accessors.size());
                    tinygltf::Accessor a; a.bufferView=-1; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                    a.type=TINYGLTF_TYPE_VEC2; a.count=static_cast<int>(vtxCount);
                    model.accessors.push_back(std::move(a)); prim.attributes["TEXCOORD_0"]=aUV; }
                int aIdx=static_cast<int>(model.accessors.size());
                { tinygltf::Accessor a; a.bufferView=-1;
                  a.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
                  a.type=TINYGLTF_TYPE_SCALAR; a.count=static_cast<int>(node.indices.size());
                  model.accessors.push_back(std::move(a)); }
                prim.indices=aIdx;
                prim.extensions=makeDracoExt(bvD,dr.posAttrId,dr.uvAttrId);
                mesh.primitives.push_back(std::move(prim));
                dracoUsed=true;
            }
        }
#endif
        if (!dracoUsed) {
            int bvVtx=addBV(node.vertices.data(),node.vertices.size()*4,TINYGLTF_TARGET_ARRAY_BUFFER);
            int bvUV=-1;
            if (!node.uvs.empty()) bvUV=addBV(node.uvs.data(),node.uvs.size()*4,TINYGLTF_TARGET_ARRAY_BUFFER);
            int bvIdx=addBV(node.indices.data(),node.indices.size()*4,TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            int aVtx=static_cast<int>(model.accessors.size());
            { tinygltf::Accessor a; a.bufferView=bvVtx; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
              a.type=TINYGLTF_TYPE_VEC3; a.count=static_cast<int>(vtxCount);
              a.minValues=posMin; a.maxValues=posMax; model.accessors.push_back(std::move(a)); }
            prim.attributes["POSITION"]=aVtx;
            if (bvUV>=0) {
                int aUV=static_cast<int>(model.accessors.size());
                tinygltf::Accessor a; a.bufferView=bvUV; a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
                a.type=TINYGLTF_TYPE_VEC2; a.count=static_cast<int>(vtxCount);
                model.accessors.push_back(std::move(a)); prim.attributes["TEXCOORD_0"]=aUV; }
            int aIdx=static_cast<int>(model.accessors.size());
            { tinygltf::Accessor a; a.bufferView=bvIdx;
              a.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
              a.type=TINYGLTF_TYPE_SCALAR; a.count=static_cast<int>(node.indices.size());
              model.accessors.push_back(std::move(a)); }
            prim.indices=aIdx;
            mesh.primitives.push_back(std::move(prim));
        }
    }

    if (mesh.primitives.empty()) {
        LOG_WARN("No primitives for: " + outPath);
        return false;
    }

    model.buffers.push_back(std::move(buf));
    model.meshes.push_back(std::move(mesh));

    // ── Node + Scene ─────────────────────────────────────────────
    tinygltf::Node gltfNode;
    gltfNode.mesh = 0;
    model.nodes.push_back(std::move(gltfNode));
    scene.nodes.push_back(0);
    model.scenes.push_back(std::move(scene));
    model.defaultScene = 0;

    model.asset.version   = "2.0";
    model.asset.generator = "osgb2tiles 1.0";

    // ── 写入 GLB ─────────────────────────────────────────────────
    tinygltf::TinyGLTF gltf;
    bool ok = gltf.WriteGltfSceneToFile(&model, outPath,
                                         true,   // embedImages
                                         true,   // embedBuffers
                                         false,  // prettyPrint
                                         true);  // writeBinary
    if (!ok) LOG_ERROR("Failed to write GLB: " + outPath);
    return ok;
}



bool GlbWriter::writeB3dm(const TileNode& node,
                           const std::vector<uint8_t>& texData,
                           const std::string& mimeType,
                           const std::string& outPath)
{
    // 先生成 GLB 到内存（使用临时文件路径）
    std::string tmpGlb = outPath + ".tmp.glb";
    if (!writeGlb(node, texData, mimeType, tmpGlb)) return false;

    // 读取 GLB
    std::ifstream in(tmpGlb, std::ios::binary);
    std::vector<uint8_t> glbData((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    in.close();
    fs::remove(tmpGlb);

    // ── B3DM 头组装 ─────────────────────────────────────
    // FeatureTable JSON：包含 RTC_CENTER（参考数据标准做法）
    // RTC_CENTER 必须是 3 个元素的数组（ECEF 坐标）
    // 由于我们的顶点已居中到 ENU 原点，这里填入原点的 ECEF坐标
    // （即 tileset.json transform 矩阵的平移分量）
    json ftJson;
    ftJson["BATCH_LENGTH"] = 0;
    // Pattern 1: 没有逆 RTC，顺序 ECEF坐标由 tileset transform 处理
    // 不设置 RTC_CENTER（顶点是本地 ENU，不需要 RTC 偏移）
    std::string ftStr = ftJson.dump();
    while (ftStr.size() % 8 != 0) ftStr += ' ';

    // BatchTable 为空
    std::string btStr = "{}";
    while (btStr.size() % 8 != 0) btStr += ' ';


    uint32_t featureTableJSONByteLength  = static_cast<uint32_t>(ftStr.size());
    uint32_t featureTableBinaryByteLength = 0;
    uint32_t batchTableJSONByteLength    = static_cast<uint32_t>(btStr.size());
    uint32_t batchTableBinaryByteLength  = 0;

    uint32_t headerLen = 28;
    uint32_t totalLen  = headerLen
                       + featureTableJSONByteLength
                       + featureTableBinaryByteLength
                       + batchTableJSONByteLength
                       + batchTableBinaryByteLength
                       + static_cast<uint32_t>(glbData.size());

    std::vector<uint8_t> b3dm;
    b3dm.reserve(totalLen);

    auto appendBytes = [&](const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        b3dm.insert(b3dm.end(), p, p + size);
    };
    auto appendU32 = [&](uint32_t v) { appendBytes(&v, 4); };

    appendBytes(B3DM_MAGIC, 4);
    appendU32(B3DM_VERSION);
    appendU32(totalLen);
    appendU32(featureTableJSONByteLength);
    appendU32(featureTableBinaryByteLength);
    appendU32(batchTableJSONByteLength);
    appendU32(batchTableBinaryByteLength);
    appendBytes(ftStr.data(), ftStr.size());
    appendBytes(btStr.data(), btStr.size());
    appendBytes(glbData.data(), glbData.size());

    return writeBytes(outPath, b3dm);
}

bool GlbWriter::writeBytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        LOG_ERROR("Cannot open for writing: " + path);
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return ofs.good();
}

void GlbWriter::pad4(std::vector<uint8_t>& buf, uint8_t padByte) {
    while (buf.size() % 4 != 0) buf.push_back(padByte);
}
