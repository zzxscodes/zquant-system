#pragma once

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

#include "me_order_book.h"

namespace Exchange {
  // 匹配引擎类，负责处理客户端订单请求、执行订单匹配并生成响应和市场数据更新
  class MatchingEngine final {
  public:
    MatchingEngine(ClientRequestLFQueue *client_requests,
                   ClientResponseLFQueue *client_responses,
                   MEMarketUpdateLFQueue *market_updates);

    ~MatchingEngine();

    auto start() -> void;
    auto stop() -> void;

    // 处理从无锁队列读取的客户端请求（由订单服务器发送）
    auto processClientRequest(const MEClientRequest *client_request) noexcept {
      auto order_book = ticker_order_book_[client_request->ticker_id_];  // 获取对应股票的订单簿
      switch (client_request->type_) {
        case ClientRequestType::NEW: {
          // 添加新订单到订单簿
          START_MEASURE(Exchange_MEOrderBook_add);
          order_book->add(client_request->client_id_, client_request->order_id_, client_request->ticker_id_,
                           client_request->side_, client_request->price_, client_request->qty_);
          END_MEASURE(Exchange_MEOrderBook_add, logger_);
        }
          break;

        case ClientRequestType::CANCEL: {
          // 从订单簿取消订单
          START_MEASURE(Exchange_MEOrderBook_cancel);
          order_book->cancel(client_request->client_id_, client_request->order_id_, client_request->ticker_id_);
          END_MEASURE(Exchange_MEOrderBook_cancel, logger_);
        }
          break;

        default: {
          FATAL("收到无效的客户端请求类型：" + clientRequestTypeToString(client_request->type_));
        }
          break;
      }
    }

    // 将客户端响应写入无锁队列，供订单服务器消费
    auto sendClientResponse(const MEClientResponse *client_response) noexcept {
      logger_.log("%:% %() % 发送 %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), client_response->toString());
      auto next_write = outgoing_ogw_responses_->getNextToWriteTo();
      *next_write = std::move(*client_response);
      outgoing_ogw_responses_->updateWriteIndex();  // 更新队列写入索引
      TTT_MEASURE(T4t_MatchingEngine_LFQueue_write, logger_);  // 测量队列写入时间
    }

    // 将市场数据更新写入无锁队列，供市场数据发布器消费
    auto sendMarketUpdate(const MEMarketUpdate *market_update) noexcept {
      logger_.log("%:% %() % 发送 %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), market_update->toString());
      auto next_write = outgoing_md_updates_->getNextToWriteTo();
      *next_write = *market_update;
      outgoing_md_updates_->updateWriteIndex();  // 更新队列写入索引
      TTT_MEASURE(T4_MatchingEngine_LFQueue_write, logger_);  // 测量队列写入时间
    }

    // 处理传入的客户端请求，生成客户端响应和市场更新
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      while (run_) {
        const auto me_client_request = incoming_requests_->getNextToRead();  // 读取请求
        if (LIKELY(me_client_request)) {
          TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);  // 测量队列读取时间

          logger_.log("%:% %() % 处理 %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                      me_client_request->toString());
          START_MEASURE(Exchange_MatchingEngine_processClientRequest);
          processClientRequest(me_client_request);  // 处理请求
          END_MEASURE(Exchange_MatchingEngine_processClientRequest, logger_);  // 测量处理时间
          incoming_requests_->updateReadIndex();  // 更新队列读取索引
        }
      }
    }

    MatchingEngine() = delete;
    MatchingEngine(const MatchingEngine &) = delete;
    MatchingEngine(const MatchingEngine &&) = delete;
    MatchingEngine &operator=(const MatchingEngine &) = delete;
    MatchingEngine &operator=(const MatchingEngine &&) = delete;

  private:
    // 从股票代码（TickerId）到MEOrderBook的哈希映射容器
    OrderBookHashMap ticker_order_book_;

    // 无锁队列：
    // 一个用于消费订单服务器发送的传入客户端请求
    // 第二个用于发布 outgoing 客户端响应，供订单服务器消费
    // 第三个用于发布 outgoing 市场更新，供市场数据发布器消费
    ClientRequestLFQueue *incoming_requests_ = nullptr;
    ClientResponseLFQueue *outgoing_ogw_responses_ = nullptr;
    MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;
  };
}