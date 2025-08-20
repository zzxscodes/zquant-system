#include "market_data_publisher.h"

namespace Exchange {
  MarketDataPublisher::MarketDataPublisher(MEMarketUpdateLFQueue *market_updates, const std::string &iface,
                                           const std::string &snapshot_ip, int snapshot_port,
                                           const std::string &incremental_ip, int incremental_port)
      : outgoing_md_updates_(market_updates), snapshot_md_updates_(ME_MAX_MARKET_UPDATES),
        run_(false), logger_("exchange_market_data_publisher.log"), incremental_socket_(logger_) {
    // 初始化增量数据多播 socket
    ASSERT(incremental_socket_.init(incremental_ip, iface, incremental_port, /*is_listening*/ false) >= 0,
           "无法创建增量多播 socket。错误：" + std::string(std::strerror(errno)));
    // 创建快照合成器
    snapshot_synthesizer_ = new SnapshotSynthesizer(&snapshot_md_updates_, iface, snapshot_ip, snapshot_port);
  }

  // 从无锁队列消费匹配引擎的市场更新，发布到增量多播流，并转发给快照合成器
  auto MarketDataPublisher::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      // 读取并处理所有待处理的市场更新
      for (auto market_update = outgoing_md_updates_->getNextToRead();
           outgoing_md_updates_->size() && market_update; market_update = outgoing_md_updates_->getNextToRead()) {
        TTT_MEASURE(T5_MarketDataPublisher_LFQueue_read, logger_);  // 测量队列读取时间

        logger_.log("%:% %() % 发送序列号：% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), next_inc_seq_num_,
                    market_update->toString().c_str());

        // 发送增量数据序列号和市场更新内容
        START_MEASURE(Exchange_McastSocket_send);
        incremental_socket_.send(&next_inc_seq_num_, sizeof(next_inc_seq_num_));
        incremental_socket_.send(market_update, sizeof(MEMarketUpdate));
        END_MEASURE(Exchange_McastSocket_send, logger_);  // 测量发送时间

        outgoing_md_updates_->updateReadIndex();  // 更新队列读取索引
        TTT_MEASURE(T6_MarketDataPublisher_UDP_write, logger_);  // 测量 UDP 写入时间

        // 将增量市场数据更新转发给快照合成器
        auto next_write = snapshot_md_updates_.getNextToWriteTo();
        next_write->seq_num_ = next_inc_seq_num_;
        next_write->me_market_update_ = *market_update;
        snapshot_md_updates_.updateWriteIndex();  // 更新快照队列写入索引

        ++next_inc_seq_num_;  // 递增增量数据序列号
      }

      // 发布数据到多播流
      incremental_socket_.sendAndRecv();
    }
  }
}