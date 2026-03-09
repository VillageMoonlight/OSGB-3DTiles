#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""patch_main_fix.py: fix deadlock by using std::thread for Stage2 extractors"""

NEW_SECTION = """\
  // 3. Three-stage pipeline:
  //   Stage1: 1 std::thread  - osgDB::readNodeFile (OSG global lock, serial)
  //   Stage2: N std::threads - extractFromNode, parallel JPEG decode (no pool, avoids deadlock)
  //   Stage3: N pool threads - simplify/encode/write (uses pool)
  size_t numWorkers = std::max<size_t>(1, pool.size());
  std::string ext   = (cfg.tileFormat == "b3dm") ? ".b3dm" : ".glb";

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

  // Stage2: N independent std::threads for geometry+JPEG decode
  // NOTE: Must NOT use pool.submit() here - would deadlock if pool is full with Stage2 tasks
  //       while Stage3 tasks are waiting in pool queue and workQueue is full.
  std::vector<std::thread> extractThreads;
  extractThreads.reserve(numWorkers);
  for (size_t wi = 0; wi < numWorkers; ++wi) {
    extractThreads.emplace_back([&]() {
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
    });
  }

  // Stage3: N pool workers, parallel simplify+encode+write
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

  // Wait order: Reader -> Stage2 threads -> close workQueue -> Stage3
  readerThread.join();
  for (auto &t : extractThreads) t.join();
  workQueue.setDone();
  for (auto &f : workerFuts) f.get();

  totalConverted += ok;\
"""

CPP_FILE = r'c:\Users\hubiao\Desktop\AIStudio\AntiGravity\OSGB-3DTiles\src\main.cpp'

with open(CPP_FILE, 'r', encoding='utf-8') as f:
    content = f.read()

# Markers that exist in the post-patch file
old_start = '// 3. Three-stage pipeline for maximum parallelism:'
old_end   = 'totalConverted += ok;'

start_idx = content.find(old_start)
if start_idx < 0:
    print('ERROR: old_start not found!')
    exit(1)

end_idx = content.find(old_end, start_idx)
if end_idx < 0:
    print('ERROR: old_end not found!')
    exit(1)
end_idx += len(old_end)

print(f'Replacing [{start_idx}, {end_idx}] (len={end_idx - start_idx})')
new_content = content[:start_idx] + NEW_SECTION + content[end_idx:]

with open(CPP_FILE, 'w', encoding='utf-8') as f:
    f.write(new_content)
print('patch_main_fix.py: Done!')
