#include "snapshot_synthesizer.h"

namespace Exchange {
  SnapshotSynthesizer::SnapshotSynthesizer(MDPMarketUpdateLFQueue *market_updates, const std::string &iface,
                                           const std::string &snapshot_ip, int snapshot_port)
      : snapshot_md_updates_(market_updates), logger_("exchange_snapshot_synthesizer.log"), snapshot_socket_(logger_), order_pool_(ME_MAX_ORDER_IDS) {
    // 初始化快照多播 socket
    ASSERT(snapshot_socket_.init(snapshot_ip, iface, snapshot_port, /*is_listening*/ false) >= 0,
           "无法创建快照多播 socket。错误：" + std::string(std::strerror(errno)));
    // 初始化股票订单数组（全部置空）
    for(auto& orders : ticker_orders_)
      orders.fill(nullptr);
  }

  SnapshotSynthesizer::~SnapshotSynthesizer() {
    stop();
  }

  // 启动和停止快照合成器线程
  void SnapshotSynthesizer::start() {
    run_ = true;
    ASSERT(Common::createAndStartThread(-1, "Exchange/SnapshotSynthesizer", [this]() { run(); }) != nullptr,
           "无法启动 SnapshotSynthesizer 线程。");
  }

  void SnapshotSynthesizer::stop() {
    run_ = false;
  }

  // 处理增量市场更新并更新限价订单簿快照
  auto SnapshotSynthesizer::addToSnapshot(const MDPMarketUpdate *market_update) {
    const auto &me_market_update = market_update->me_market_update_;
    auto *orders = &ticker_orders_.at(me_market_update.ticker_id_);  // 获取对应股票的订单数组

    // 根据市场更新类型处理
    switch (me_market_update.type_) {
      case MarketUpdateType::ADD: {
        auto order = orders->at(me_market_update.order_id_);
        // 断言：添加的订单不存在
        ASSERT(order == nullptr, "收到：" + me_market_update.toString() + " 但订单已存在：" + (order ? order->toString() : ""));
        // 从内存池分配订单并存储
        orders->at(me_market_update.order_id_) = order_pool_.allocate(me_market_update);
      }
        break;
      case MarketUpdateType::MODIFY: {
        auto order = orders->at(me_market_update.order_id_);
        // 断言：修改的订单存在且信息匹配
        ASSERT(order != nullptr, "收到：" + me_market_update.toString() + " 但订单不存在。");
        ASSERT(order->order_id_ == me_market_update.order_id_, "预期现有订单与新订单匹配。");
        ASSERT(order->side_ == me_market_update.side_, "预期现有订单与新订单匹配。");

        // 更新订单数量和价格
        order->qty_ = me_market_update.qty_;
        order->price_ = me_market_update.price_;
      }
        break;
      case MarketUpdateType::CANCEL: {
        auto order = orders->at(me_market_update.order_id_);
        // 断言：取消的订单存在且信息匹配
        ASSERT(order != nullptr, "收到：" + me_market_update.toString() + " 但订单不存在。");
        ASSERT(order->order_id_ == me_market_update.order_id_, "预期现有订单与新订单匹配。");
        ASSERT(order->side_ == me_market_update.side_, "预期现有订单与新订单匹配。");

        // 释放订单并置空
        order_pool_.deallocate(order);
        orders->at(me_market_update.order_id_) = nullptr;
      }
        break;
      // 忽略快照相关、清除、成交和无效类型的更新
      case MarketUpdateType::SNAPSHOT_START:
      case MarketUpdateType::CLEAR:
      case MarketUpdateType::SNAPSHOT_END:
      case MarketUpdateType::TRADE:
      case MarketUpdateType::INVALID:
        break;
    }

    // 断言：增量序列号连续递增
    ASSERT(market_update->seq_num_ == last_inc_seq_num_ + 1, "预期增量序列号递增。");
    last_inc_seq_num_ = market_update->seq_num_;  // 更新最后处理的序列号
  }

  // 在快照多播流上发布完整的快照周期
  auto SnapshotSynthesizer::publishSnapshot() {
    size_t snapshot_size = 0;  // 快照包含的更新数量

    // 快照周期以 SNAPSHOT_START 消息开始，order_id_ 包含用于构建此快照的增量市场数据流的最后序列号
    const MDPMarketUpdate start_market_update{snapshot_size++, {MarketUpdateType::SNAPSHOT_START, last_inc_seq_num_}};
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_), start_market_update.toString());
    snapshot_socket_.send(&start_market_update, sizeof(MDPMarketUpdate));  // 发送开始消息

    // 为每个工具的限价订单簿中的每个订单发布订单信息
    for (size_t ticker_id = 0; ticker_id < ticker_orders_.size(); ++ticker_id) {
      const auto &orders = ticker_orders_.at(ticker_id);  // 获取对应股票的订单数组

      MEMarketUpdate me_market_update;
      me_market_update.type_ = MarketUpdateType::CLEAR;
      me_market_update.ticker_id_ = ticker_id;

      // 发布每个工具的订单信息前，先发布 CLEAR 消息，以便下游消费者清空订单簿
      const MDPMarketUpdate clear_market_update{snapshot_size++, me_market_update};
      logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_), clear_market_update.toString());
      snapshot_socket_.send(&clear_market_update, sizeof(MDPMarketUpdate));  // 发送清除消息

      // 发布每个订单
      for (const auto order: orders) {
        if (order) {
          const MDPMarketUpdate market_update{snapshot_size++, *order};
          logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_), market_update.toString());
          snapshot_socket_.send(&market_update, sizeof(MDPMarketUpdate));  // 发送订单信息
          snapshot_socket_.sendAndRecv();  // 处理发送和接收
        }
      }
    }

    // 快照周期以 SNAPSHOT_END 消息结束，order_id_ 包含用于构建此快照的增量市场数据流的最后序列号
    const MDPMarketUpdate end_market_update{snapshot_size++, {MarketUpdateType::SNAPSHOT_END, last_inc_seq_num_}};
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_), end_market_update.toString());
    snapshot_socket_.send(&end_market_update, sizeof(MDPMarketUpdate));  // 发送结束消息
    snapshot_socket_.sendAndRecv();  // 处理发送和接收

    logger_.log("%:% %() % 已发布包含 % 个订单的快照。\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_), snapshot_size - 1);
  }

  // 处理来自市场数据发布器的增量更新，更新快照并定期发布快照
  void SnapshotSynthesizer::run() {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_));
    while (run_) {
      // 处理所有待处理的市场更新
      for (auto market_update = snapshot_md_updates_->getNextToRead(); snapshot_md_updates_->size() && market_update; market_update = snapshot_md_updates_->getNextToRead()) {
        logger_.log("%:% %() % 处理 %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_),
                    market_update->toString().c_str());

        addToSnapshot(market_update);  // 更新快照

        snapshot_md_updates_->updateReadIndex();  // 更新队列读取索引
      }

      // 每60秒发布一次快照
      if (getCurrentNanos() - last_snapshot_time_ > 60 * NANOS_TO_SECS) {
        last_snapshot_time_ = getCurrentNanos();  // 更新最后快照时间
        publishSnapshot();  // 发布快照
      }
    }
  }
}