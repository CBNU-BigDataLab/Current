/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>
          (c) 2015 Maxim Zhurovich <zhurovich@gmail.com>

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

#include "mq.h"

#include "../../../3rdparty/gtest/gtest-main.h"
#include "../../strings/printf.h"

#include <atomic>
#include <thread>
#include <chrono>

using bricks::mq::MMQ;

TEST(InMemoryMQ, SmokeTest) {
  struct Consumer {
    std::string messages_;
    size_t expected_next_message_index_ = 0u;
    size_t dropped_messages_ = 0u;
    std::atomic_size_t processed_messages_;
    Consumer() : processed_messages_(0u) {}
    void operator()(const std::string& s, size_t index) {
      assert(index >= expected_next_message_index_);
      dropped_messages_ += (index - expected_next_message_index_);
      expected_next_message_index_ = (index + 1);
      messages_ += s + '\n';
      ++processed_messages_;
      assert(expected_next_message_index_ - processed_messages_ == dropped_messages_);
    }
  };

  Consumer c;
  MMQ<std::string, Consumer> mmq(c);
  mmq.PushMessage("one");
  mmq.PushMessage("two");
  mmq.PushMessage("three");
  while (c.processed_messages_ != 3) {
    ;  // Spin lock;
  }
  EXPECT_EQ("one\ntwo\nthree\n", c.messages_);
  EXPECT_EQ(0u, c.dropped_messages_);
}

struct SuspendableConsumer {
  std::vector<std::string> messages_;
  std::atomic_size_t processed_messages_;
  size_t total_messages_pushed_into_the_queue_ = 0u;
  bool observed_gap_in_message_indexes_ = false;
  std::atomic_bool suspend_processing_;
  size_t processing_delay_ms_ = 0u;
  SuspendableConsumer() : processed_messages_(0u), suspend_processing_(false) {}
  void operator()(const std::string& s, size_t index, size_t total) {
    while (suspend_processing_) {
      ;  // Spin lock.
    }
    observed_gap_in_message_indexes_ |= (index != processed_messages_);
    messages_.push_back(s);
    assert(total >= total_messages_pushed_into_the_queue_);
    total_messages_pushed_into_the_queue_ = total;
    if (processing_delay_ms_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(processing_delay_ms_));
    }
    ++processed_messages_;
  }
  void SetProcessingDelayMillis(uint64_t delay_ms) { processing_delay_ms_ = delay_ms; }
};

TEST(InMemoryMQ, DropOnOverflowTest) {
  SuspendableConsumer c;

  // Queue with 10 at most messages in the buffer.
  MMQ<std::string, SuspendableConsumer, 10, true> mmq(c);

  // Suspend the consumer temporarily while the first 25 messages are pushed.
  c.suspend_processing_ = true;

  // Push 25 messages, causing an overflow, of which 15 will be discarded.
  size_t messages_pushed = 0u;
  size_t messages_discarded = 0u;
  for (size_t i = 0; i < 25; ++i) {
    if (mmq.PushMessage(bricks::strings::Printf("M%02d", static_cast<int>(i)))) {
      ++messages_pushed;
    } else {
      ++messages_discarded;
    }
  }

  // Confirm that 10/25 messages were pushed, and 15/25 were discared.
  EXPECT_EQ(10u, messages_pushed);
  EXPECT_EQ(15u, messages_discarded);

  // Confirm that the consumer did not yet observe that some messages were discarded.
  EXPECT_FALSE(c.observed_gap_in_message_indexes_);

  // Eliminate processing delay and wait until the complete queue of 10 messages is played through.
  c.suspend_processing_ = false;
  while (c.processed_messages_ != 10u) {
    ;  // Spin lock.
  }

  // Now, to have the consumer observe the index and the counter of the messages,
  // and note that 15 messages, with 0-based indexes [10 .. 25), have not been seen.
  mmq.PushMessage("Plus one");
  while (c.processed_messages_ != 11u) {
    ;  // Spin lock.
  }

  // Now, since the consumer will see messages with 0-based index `25`
  // right after the one with zero-based index `9`, it will observe the gap.
  EXPECT_TRUE(c.observed_gap_in_message_indexes_);
  EXPECT_EQ(c.messages_.size(), 11u);
  EXPECT_EQ(c.total_messages_pushed_into_the_queue_, 26u);

  // Confirm that 11 messages have reached the consumer: first 10/25 and one more later.
  // Also confirm they are all unique.
  EXPECT_EQ(11u, c.messages_.size());
  EXPECT_EQ(11u, std::set<std::string>(begin(c.messages_), end(c.messages_)).size());
}

TEST(InMemoryMQ, WaitOnOverflowTest) {
  SuspendableConsumer c;
  c.SetProcessingDelayMillis(1u);

  // Queue with 10 events in the buffer.
  MMQ<std::string, SuspendableConsumer, 10> mmq(c);

  const auto producer = [&](char prefix, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      mmq.PushMessage(bricks::strings::Printf("%c%02d", prefix, static_cast<int>(i)));
    }
  };

  std::vector<std::thread> producers;

  for (size_t i = 0; i < 10; ++i) {
    producers.emplace_back(producer, 'a' + i, 10);
  }

  for (auto& p : producers) {
    p.join();
  }

  // Since we push 100 messages and the size of the buffer is 10,
  // we must see at least 90 messages processed by this moment.
  EXPECT_GE(c.processed_messages_, 90u);

  // Wait until the rest of the queued messages are processed.
  while (c.processed_messages_ != 100u) {
    ;  // Spin lock;
  }

  // Confirm that none of the messages were dropped.
  EXPECT_EQ(c.processed_messages_, c.total_messages_pushed_into_the_queue_);

  // Ensure that all processed messages are indeed unique.
  std::set<std::string> messages(begin(c.messages_), end(c.messages_));
  EXPECT_EQ(100u, std::set<std::string>(c.messages_.begin(), c.messages_.end()).size());
}