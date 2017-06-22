/*******************************************************************************
 The MIT License (MIT)

 Copyright (c) 2017 Grigory Nikolaenko <nikolaenko.grigory@gmail.com>

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 *******************************************************************************/

#include "../../../Bricks/dflags/dflags.h"
#include "../../../Sherlock/replicator.h"

#include "entry.h"

DEFINE_string(url, "127.0.0.1:8383/raw_log", "Url to subscribe to.");
DEFINE_string(db, "replicated_data.json", "Path to load the source stream data from.");
DEFINE_uint32(total_entries, 10000, "Entries number to replicate.");

template <typename STREAM, typename... ARGS>
void Replicate(ARGS&&... args) {
  STREAM replicated_stream(std::forward<ARGS>(args)...);
  std::cout << "Connecting to the stream at '" << FLAGS_url << "' ..." << std::flush;
  current::sherlock::SubscribableRemoteStream<benchmark::replication::Entry> remote_stream(FLAGS_url);
  auto replicator = std::make_unique<current::sherlock::StreamReplicator<STREAM>>(replicated_stream);

  const auto start_time = std::chrono::system_clock::now();
  {
    std::cout << "\b\b\bOK\nSubscribing to the stream ..." << std::flush;
    const auto subscriber_scope = remote_stream.Subscribe(*replicator);
    std::cout << "\b\b\bOK" << std::endl;
    auto next_print_time = start_time + std::chrono::milliseconds(100);
    while (replicated_stream.Persister().Size() != FLAGS_total_entries) {
      std::this_thread::yield();
      if (std::chrono::system_clock::now() >= next_print_time) {
        next_print_time += std::chrono::milliseconds(100);
        std::cout << "\rReplicated " << replicated_stream.Persister().Size() << " of " << FLAGS_total_entries << " entries" << std::flush;
      }
    }
  }
  const auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start_time).count();
  std::cout << "\rReplication filished, total time: " << duration / 1000.0 << " seconds." << std::endl;
}

int main(int argc, char** argv) {
  ParseDFlags(&argc, &argv);
  if (!FLAGS_db.empty()) {
    const auto filename = current::FileSystem::GenTmpFileName();
    const auto replicated_stream_file_remover = current::FileSystem::ScopedRmFile(filename);
    Replicate<current::sherlock::Stream<benchmark::replication::Entry, current::persistence::File>>(filename);
  } else {
    Replicate<current::sherlock::Stream<benchmark::replication::Entry, current::persistence::Memory>>();
  }
  return 0;
}
