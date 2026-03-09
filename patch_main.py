#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""patch_main.py: replace 2-stage pipeline with 3-stage pipeline in main.cpp"""

NEW_SECTION = """\
  // 3. Three-stage pipeline for maximum parallelism:
  //   Stage1: single-thread osgDB::readNodeFile (OSG global lock, must be serial)
  //   Stage2: N-thread parallel geometry+JPEG decode (no OSG lock, CPU-intensive)
  //   Stage3: N-thread parallel simplify/encode/write
  size_t numWorkers = std::max<size_t>(1, pool.size());
  std::string ext   = (cfg.tileFormat == "b3dm") ? ".b3dm" : ".glb";

  // Stage1->Stage2 queue (osg::Node ref + paths)
  struct NodeItem {
    osg::ref_ptr<osg::Node> osgNode;
    std::string             osgbPath;
    std::string             outFile;
  };
  BoundedQueue<NodeItem> nodeQueue(numWorkers * 2);
  BoundedQueue<WorkItem> workQueue(numWorkers * 3);

  // Stage1: Reader single thread, only osgDB::readNodeFile
  std::thread readerThread([&]() {
    OsgbReader reader1(cfg.verbose);
    for (const auto &osgbPath : allOsgbPaths) {
      if (!fs::exists(osgbPath)) continue;
      auto osgNode = reader1.readFile(osgbPath);
      if (osgNode.valid()) {
        std::string stem = fs::path(osgbPath).stem().string();
        nodeQueue.push(NodeItem{std::move(osgNode), osgbPath,
                                blockOutDir + "/" + stem + ext});
      }
    }
    nodeQueue.setDone();
  });

  // Stage2: N Extractor Workers, parallel geometry+JPEG decode (no OSG lock)
  std::vector<std::future<void>> extractFuts;
  for (size_t wi = 0; wi < numWorkers; ++wi) {
    extractFuts.emplace_back(pool.submit([&]() {
      OsgbReader extractor(cfg.verbose);
      NodeItem nitem;
      while (nodeQueue.pop(nitem)) {
        WorkItem witem;
        witem.node.nodeId   = fs::path(nitem.osgbPath).stem().string();
        witem.node.osgbPath = nitem.osgbPath;
        witem.node.level    = 0;
        witem.outFile       = nitem.outFile;
        if (extractor.extractFromNode(nitem.osgNode, nitem.osgbPath, witem.node)) {
          workQueue.push(std::move(witem));
        }
      }
    }));
  }

  // Stage3: N CPU Workers, parallel simplify+encode+write
  std::atomic<int> ok{0}, fail{0};
  std::vector<std::future<void>> workerFuts;
  for (size_t wi = 0; wi < numWorkers; ++wi) {
    workerFuts.emplace_back(pool.submit([&]() {
      WorkItem item;
      while (workQueue.pop(item)) {
        if (processAlreadyLoaded(item.node, item.outFile, cfg))
          ++ok;
        else
          ++fail;
      }
    }));
  }

  // Wait order: Reader -> Extractors -> close workQueue -> Workers
  readerThread.join();
  for (auto &f : extractFuts) f.get();
  workQueue.setDone();
  for (auto &f : workerFuts) f.get();

  totalConverted += ok;\
"""

CPP_FILE = r'c:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles\src\main.cpp'

with open(CPP_FILE, 'r', encoding='utf-8') as f:
    content = f.read()

old_start_marker = 'BoundedQueue<WorkItem> workQueue(queueCap);'
old_end_marker   = 'totalConverted += ok;'

start_idx = content.find(old_start_marker)
if start_idx < 0:
    print('ERROR: old_start_marker not found!')
    exit(1)

# Go back to find the '// 3.' comment above
comment_idx = content.rfind('// 3.', 0, start_idx)
if comment_idx < 0:
    print('ERROR: // 3. comment not found before marker!')
    exit(1)

end_idx = content.find(old_end_marker, start_idx)
if end_idx < 0:
    print('ERROR: old_end_marker not found!')
    exit(1)
end_idx += len(old_end_marker)

print(f'Replacing range [{comment_idx}, {end_idx}] (len={end_idx-comment_idx})')
new_content = content[:comment_idx] + NEW_SECTION + content[end_idx:]

with open(CPP_FILE, 'w', encoding='utf-8') as f:
    f.write(new_content)

print('patch_main.py: Done!')
