#pragma once

#include "common/types.h"
#include "common/mem_pool.h"
#include "common/logging.h"

#include "market_order.h"
#include "exchange/market_data/market_update.h"

namespace Trading {
  class TradeEngine;

  // 市场订单簿类，用于维护和管理特定股票的限价订单
  class MarketOrderBook final {
  public:
    MarketOrderBook(TickerId ticker_id, Logger *logger);
    ~MarketOrderBook();

    // 处理市场数据更新并更新限价订单簿
    auto onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> void;

    // 设置交易引擎（父对象）
    auto setTradeEngine(TradeEngine *trade_engine) {
      trade_engine_ = trade_engine;
    }

    // 更新最优买卖报价（BBO），两个布尔参数分别表示买卖双方是否需要更新
    auto updateBBO(bool update_bid, bool update_ask) noexcept {
      if(update_bid) {
        if(bids_by_price_) {
          bbo_.bid_price_ = bids_by_price_->price_;
          bbo_.bid_qty_ = bids_by_price_->first_mkt_order_->qty_;
          // 累加该价格层级的所有订单数量
          for(auto order = bids_by_price_->first_mkt_order_->next_order_; order != bids_by_price_->first_mkt_order_; order = order->next_order_)
            bbo_.bid_qty_ += order->qty_;
        }
        else {
          bbo_.bid_price_ = Price_INVALID;
          bbo_.bid_qty_ = Qty_INVALID;
        }
      }

      if(update_ask) {
        if(asks_by_price_) {
          bbo_.ask_price_ = asks_by_price_->price_;
          bbo_.ask_qty_ = asks_by_price_->first_mkt_order_->qty_;
          // 累加该价格层级的所有订单数量
          for(auto order = asks_by_price_->first_mkt_order_->next_order_; order != asks_by_price_->first_mkt_order_; order = order->next_order_)
            bbo_.ask_qty_ += order->qty_;
        }
        else {
          bbo_.ask_price_ = Price_INVALID;
          bbo_.ask_qty_ = Qty_INVALID;
        }
      }
    }

    // 获取当前最优买卖报价（BBO）
    auto getBBO() const noexcept -> const BBO* {
      return &bbo_;
    }

    // 将订单簿信息转换为字符串（支持详细模式和有效性检查）
    auto toString(bool detailed, bool validity_check) const -> std::string;

    // 删除默认、复制和移动构造函数以及赋值运算符
    MarketOrderBook() = delete;

    MarketOrderBook(const MarketOrderBook &) = delete;

    MarketOrderBook(const MarketOrderBook &&) = delete;

    MarketOrderBook &operator=(const MarketOrderBook &) = delete;

    MarketOrderBook &operator=(const MarketOrderBook &&) = delete;

  private:
    const TickerId ticker_id_;  // 股票代码

    // 拥有此限价订单簿的父交易引擎，用于在订单簿变化或交易发生时发送通知
    TradeEngine *trade_engine_ = nullptr;

    OrderHashMap oid_to_order_;  // 从订单ID到订单对象的哈希映射

    // 用于管理MarketOrdersAtPrice对象的内存池
    MemPool<MarketOrdersAtPrice> orders_at_price_pool_;

    // 指向买单和卖单价格层级的起始/最优价格/盘口的指针
    MarketOrdersAtPrice *bids_by_price_ = nullptr;
    MarketOrdersAtPrice *asks_by_price_ = nullptr;

    // 从价格到该价格层级订单集合的哈希映射
    OrdersAtPriceHashMap price_orders_at_price_;

    // 用于管理MarketOrder对象的内存池
    MemPool<MarketOrder> order_pool_;

    BBO bbo_;  // 最优买卖报价

    std::string time_str_;
    Logger *logger_ = nullptr;  // 日志器

  private:
    // 将价格转换为索引（用于哈希映射）
    auto priceToIndex(Price price) const noexcept {
      return (price % ME_MAX_PRICE_LEVELS);
    }

    // 获取指定价格对应的价格层级订单集合
    auto getOrdersAtPrice(Price price) const noexcept -> MarketOrdersAtPrice * {
      return price_orders_at_price_.at(priceToIndex(price));
    }

    // 在容器（哈希映射和价格层级双向链表）中添加新的价格层级订单集合
    auto addOrdersAtPrice(MarketOrdersAtPrice *new_orders_at_price) noexcept {
      price_orders_at_price_.at(priceToIndex(new_orders_at_price->price_)) = new_orders_at_price;

      const auto best_orders_by_price = (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_);
      if (UNLIKELY(!best_orders_by_price)) {
        // 若该方向无订单，则新价格层级成为首个层级
        (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) = new_orders_at_price;
        new_orders_at_price->prev_entry_ = new_orders_at_price->next_entry_ = new_orders_at_price;
      } else {
        auto target = best_orders_by_price;
        // 判断是否需要在目标之后添加
        bool add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                          (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
        if (add_after) {
          target = target->next_entry_;
          add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                       (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
        }
        // 找到正确的插入位置
        while (add_after && target != best_orders_by_price) {
          add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                       (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
          if (add_after)
            target = target->next_entry_;
        }

        if (add_after) { // 在目标之后添加新价格层级
          if (target == best_orders_by_price) {
            target = best_orders_by_price->prev_entry_;
          }
          new_orders_at_price->prev_entry_ = target;
          target->next_entry_->prev_entry_ = new_orders_at_price;
          new_orders_at_price->next_entry_ = target->next_entry_;
          target->next_entry_ = new_orders_at_price;
        } else { // 在目标之前添加新价格层级
          new_orders_at_price->prev_entry_ = target->prev_entry_;
          new_orders_at_price->next_entry_ = target;
          target->prev_entry_->next_entry_ = new_orders_at_price;
          target->prev_entry_ = new_orders_at_price;

          // 若新价格层级成为最优价格，则更新最优价格指针
          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ > best_orders_by_price->price_) ||
              (new_orders_at_price->side_ == Side::SELL &&
               new_orders_at_price->price_ < best_orders_by_price->price_)) {
            target->next_entry_ = (target->next_entry_ == best_orders_by_price ? new_orders_at_price
                                                                               : target->next_entry_);
            (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) = new_orders_at_price;
          }
        }
      }
    }

    // 从容器（哈希映射和价格层级双向链表）中移除指定价格的价格层级订单集合
    auto removeOrdersAtPrice(Side side, Price price) noexcept {
      const auto best_orders_by_price = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
      auto orders_at_price = getOrdersAtPrice(price);

      if (UNLIKELY(orders_at_price->next_entry_ == orders_at_price)) { // 该方向订单簿为空
        (side == Side::BUY ? bids_by_price_ : asks_by_price_) = nullptr;
      } else {
        // 调整双向链表指针
        orders_at_price->prev_entry_->next_entry_ = orders_at_price->next_entry_;
        orders_at_price->next_entry_->prev_entry_ = orders_at_price->prev_entry_;

        // 若移除的是最优价格层级，则更新最优价格指针
        if (orders_at_price == best_orders_by_price) {
          (side == Side::BUY ? bids_by_price_ : asks_by_price_) = orders_at_price->next_entry_;
        }

        orders_at_price->prev_entry_ = orders_at_price->next_entry_ = nullptr;
      }

      // 从哈希映射中移除并释放资源
      price_orders_at_price_.at(priceToIndex(price)) = nullptr;
      orders_at_price_pool_.deallocate(orders_at_price);
    }

    // 从容器中移除并释放指定订单
    auto removeOrder(MarketOrder *order) noexcept -> void {
      auto orders_at_price = getOrdersAtPrice(order->price_);

      if (order->prev_order_ == order) { // 该价格层级只有一个订单
        removeOrdersAtPrice(order->side_, order->price_);
      } else { // 移除订单并调整链表指针
        const auto order_before = order->prev_order_;
        const auto order_after = order->next_order_;
        order_before->next_order_ = order_after;
        order_after->prev_order_ = order_before;

        // 若移除的是该价格层级的首个订单，则更新首个订单指针
        if (orders_at_price->first_mkt_order_ == order) {
          orders_at_price->first_mkt_order_ = order_after;
        }

        order->prev_order_ = order->next_order_ = nullptr;
      }

      // 从订单ID映射中移除并释放订单
      oid_to_order_.at(order->order_id_) = nullptr;
      order_pool_.deallocate(order);
    }

    // 在订单所属的价格层级的FIFO队列末尾添加单个订单
    auto addOrder(MarketOrder *order) noexcept -> void {
      const auto orders_at_price = getOrdersAtPrice(order->price_);

      if (!orders_at_price) {
        // 该价格层级不存在，创建新的价格层级
        order->next_order_ = order->prev_order_ = order;
        auto new_orders_at_price = orders_at_price_pool_.allocate(order->side_, order->price_, order, nullptr, nullptr);
        addOrdersAtPrice(new_orders_at_price);
      } else {
        // 该价格层级已存在，添加到队列末尾
        auto first_order = (orders_at_price ? orders_at_price->first_mkt_order_ : nullptr);

        first_order->prev_order_->next_order_ = order;
        order->prev_order_ = first_order->prev_order_;
        order->next_order_ = first_order;
        first_order->prev_order_ = order;
      }

      // 将订单添加到订单ID映射中
      oid_to_order_.at(order->order_id_) = order;
    }
  };

  // 从股票代码到市场订单簿的哈希映射
  typedef std::array<MarketOrderBook *, ME_MAX_TICKERS> MarketOrderBookHashMap;
}