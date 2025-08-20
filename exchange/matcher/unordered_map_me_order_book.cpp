#include "unordered_map_me_order_book.h"

#include "matcher/matching_engine.h"

namespace Exchange {
  UnorderedMapMEOrderBook::UnorderedMapMEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine)
      : ticker_id_(ticker_id), matching_engine_(matching_engine), orders_at_price_pool_(ME_MAX_PRICE_LEVELS), order_pool_(ME_MAX_ORDER_IDS),
        logger_(logger) {
  }

  UnorderedMapMEOrderBook::~UnorderedMapMEOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                toString(false, true));

    matching_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
  }

  // 将具有指定参数的新主动订单与被动订单进行匹配，生成客户端响应和市场更新
  // 根据匹配结果更新被动订单，若完全匹配则移除该订单，返回主动订单的剩余数量
  auto UnorderedMapMEOrderBook::match(TickerId ticker_id, ClientId client_id, Side side, OrderId client_order_id, OrderId new_market_order_id, MEOrder* itr, Qty* leaves_qty) noexcept {
    const auto order = itr;
    const auto order_qty = order->qty_;
    const auto fill_qty = std::min(*leaves_qty, order_qty);  // 计算成交数量（取两者较小值）

    *leaves_qty -= fill_qty;  // 更新主动订单剩余量
    order->qty_ -= fill_qty;  // 更新被动订单剩余量

    // 向主动订单客户端发送成交响应
    client_response_ = {ClientResponseType::FILLED, client_id, ticker_id, client_order_id,
                        new_market_order_id, side, itr->price_, fill_qty, *leaves_qty};
    matching_engine_->sendClientResponse(&client_response_);

    // 向被动订单客户端发送成交响应
    client_response_ = {ClientResponseType::FILLED, order->client_id_, ticker_id, order->client_order_id_,
                        order->market_order_id_, order->side_, itr->price_, fill_qty, order->qty_};
    matching_engine_->sendClientResponse(&client_response_);

    // 发送成交类型的市场更新
    market_update_ = {MarketUpdateType::TRADE, OrderId_INVALID, ticker_id, side, itr->price_, fill_qty, Priority_INVALID};
    matching_engine_->sendMarketUpdate(&market_update_);

    if (!order->qty_) {  // 被动订单完全成交，需移除
      // 发送取消类型的市场更新（表示订单已完全成交）
      market_update_ = {MarketUpdateType::CANCEL, order->market_order_id_, ticker_id, order->side_,
                        order->price_, order_qty, Priority_INVALID};
      matching_engine_->sendMarketUpdate(&market_update_);

      // 从订单簿中移除该订单
      START_MEASURE(Exchange_UnorderedMapMEOrderBook_removeOrder);
      removeOrder(order);
      END_MEASURE(Exchange_UnorderedMapMEOrderBook_removeOrder, (*logger_));
    } else {  // 被动订单部分成交，需更新
      // 发送修改类型的市场更新（更新剩余数量）
      market_update_ = {MarketUpdateType::MODIFY, order->market_order_id_, ticker_id, order->side_,
                        order->price_, order->qty_, order->priority_};
      matching_engine_->sendMarketUpdate(&market_update_);
    }
  }

  // 检查新订单是否与订单簿另一侧的现有被动订单匹配，若匹配则执行匹配并返回剩余数量
  auto UnorderedMapMEOrderBook::checkForMatch(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty, Qty new_market_order_id) noexcept {
    auto leaves_qty = qty;  // 初始化剩余数量为订单原始数量

    if (side == Side::BUY) {  // 买单（主动买入，与卖单簿匹配）
      while (leaves_qty && asks_by_price_) {  // 仍有剩余数量且卖单簿非空
        const auto ask_itr = asks_by_price_->first_me_order_;  // 获取最优卖单
        if (LIKELY(price < ask_itr->price_)) {  // 买价低于最优卖价，无法匹配
          break;
        }

        // 执行匹配
        START_MEASURE(Exchange_UnorderedMapMEOrderBook_match);
        match(ticker_id, client_id, side, client_order_id, new_market_order_id, ask_itr, &leaves_qty);
        END_MEASURE(Exchange_UnorderedMapMEOrderBook_match, (*logger_));
      }
    }
    if (side == Side::SELL) {  // 卖单（主动卖出，与买单簿匹配）
      while (leaves_qty && bids_by_price_) {  // 仍有剩余数量且买单簿非空
        const auto bid_itr = bids_by_price_->first_me_order_;  // 获取最优买单
        if (LIKELY(price > bid_itr->price_)) {  // 卖价高于最优买价，无法匹配
          break;
        }

        // 执行匹配
        START_MEASURE(Exchange_UnorderedMapMEOrderBook_match);
        match(ticker_id, client_id, side, client_order_id, new_market_order_id, bid_itr, &leaves_qty);
        END_MEASURE(Exchange_UnorderedMapMEOrderBook_match, (*logger_));
      }
    }

    return leaves_qty;  // 返回剩余未成交数量
  }

  // 创建并添加新订单到订单簿，检查是否与现有被动订单匹配并执行匹配
  auto UnorderedMapMEOrderBook::add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty) noexcept -> void {
    const auto new_market_order_id = generateNewMarketOrderId();  // 生成新的市场订单ID
    // 发送订单接受响应
    client_response_ = {ClientResponseType::ACCEPTED, client_id, ticker_id, client_order_id, new_market_order_id, side, price, 0, qty};
    matching_engine_->sendClientResponse(&client_response_);

    // 检查并执行匹配
    START_MEASURE(Exchange_UnorderedMapMEOrderBook_checkForMatch);
    const auto leaves_qty = checkForMatch(client_id, client_order_id, ticker_id, side, price, qty, new_market_order_id);
    END_MEASURE(Exchange_UnorderedMapMEOrderBook_checkForMatch, (*logger_));

    if (LIKELY(leaves_qty)) {  // 若有剩余未成交数量，将剩余部分加入订单簿
      const auto priority = getNextPriority(price);  // 获取订单优先级

      // 从内存池分配订单并初始化
      auto order = order_pool_.allocate(ticker_id, client_id, client_order_id, new_market_order_id, side, price, leaves_qty, priority, nullptr,
                                        nullptr);
      // 添加订单到订单簿
      START_MEASURE(Exchange_UnorderedMapMEOrderBook_addOrder);
      addOrder(order);
      END_MEASURE(Exchange_UnorderedMapMEOrderBook_addOrder, (*logger_));

      // 发送添加类型的市场更新
      market_update_ = {MarketUpdateType::ADD, new_market_order_id, ticker_id, side, price, leaves_qty, priority};
      matching_engine_->sendMarketUpdate(&market_update_);
    }
  }

  // 尝试取消订单簿中的订单，若订单不存在则发送取消拒绝响应
  auto UnorderedMapMEOrderBook::cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void {
    auto is_cancelable = (client_id < cid_oid_to_order_.size());  // 检查客户端ID是否有效
    MEOrder *exchange_order = nullptr;
    if (LIKELY(is_cancelable)) {
      auto &co_itr = cid_oid_to_order_[client_id];  // 获取客户端对应的订单映射
      exchange_order = co_itr[order_id];  // 获取要取消的订单
      is_cancelable = (exchange_order != nullptr);  // 检查订单是否存在
    }

    if (UNLIKELY(!is_cancelable)) {  // 订单不可取消（不存在或客户端ID无效）
      client_response_ = {ClientResponseType::CANCEL_REJECTED, client_id, ticker_id, order_id, OrderId_INVALID,
                          Side::INVALID, Price_INVALID, Qty_INVALID, Qty_INVALID};
    } else {  // 订单可取消
      // 发送取消成功响应
      client_response_ = {ClientResponseType::CANCELED, client_id, ticker_id, order_id, exchange_order->market_order_id_,
                          exchange_order->side_, exchange_order->price_, Qty_INVALID, exchange_order->qty_};
      // 发送取消类型的市场更新
      market_update_ = {MarketUpdateType::CANCEL, exchange_order->market_order_id_, ticker_id, exchange_order->side_, exchange_order->price_, 0,
                        exchange_order->priority_};

      // 从订单簿中移除订单
      START_MEASURE(Exchange_UnorderedMapMEOrderBook_removeOrder);
      removeOrder(exchange_order);
      END_MEASURE(Exchange_UnorderedMapMEOrderBook_removeOrder, (*logger_));

      matching_engine_->sendMarketUpdate(&market_update_);
    }

    matching_engine_->sendClientResponse(&client_response_);  // 发送客户端响应
  }

  // 将订单簿信息转换为字符串（支持详细模式和有效性检查）
  auto UnorderedMapMEOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;
    std::string time_str;

    // 用于打印价格层级及其包含订单的lambda函数
    auto printer = [&](std::stringstream &ss, MEOrdersAtPrice *itr, Side side, Price &last_price, bool sanity_check) {
      char buf[4096];
      Qty qty = 0;  // 该价格层级的总数量
      size_t num_orders = 0;  // 该价格层级的订单数

      // 计算总数量和订单数
      for (auto o_itr = itr->first_me_order_;; o_itr = o_itr->next_order_) {
        qty += o_itr->qty_;
        ++num_orders;
        if (o_itr->next_order_ == itr->first_me_order_)
          break;
      }
      // 打印价格层级基本信息
      sprintf(buf, " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
              priceToString(itr->price_).c_str(), priceToString(itr->prev_entry_->price_).c_str(), priceToString(itr->next_entry_->price_).c_str(),
              priceToString(itr->price_).c_str(), qtyToString(qty).c_str(), std::to_string(num_orders).c_str());
      ss << buf;
      // 详细模式下打印每个订单的信息
      for (auto o_itr = itr->first_me_order_;; o_itr = o_itr->next_order_) {
        if (detailed) {
          sprintf(buf, "[oid:%s q:%s p:%s n:%s] ",
                  orderIdToString(o_itr->market_order_id_).c_str(), qtyToString(o_itr->qty_).c_str(),
                  orderIdToString(o_itr->prev_order_ ? o_itr->prev_order_->market_order_id_ : OrderId_INVALID).c_str(),
                  orderIdToString(o_itr->next_order_ ? o_itr->next_order_->market_order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o_itr->next_order_ == itr->first_me_order_)
          break;
      }

      ss << std::endl;

      // 有效性检查：确保买卖盘价格排序正确
      if (sanity_check) {
        if ((side == Side::SELL && last_price >= itr->price_) || (side == Side::BUY && last_price <= itr->price_)) {
          FATAL("Bids/Asks not sorted by ascending/descending prices last:" + priceToString(last_price) + " itr:" + itr->toString());
        }
        last_price = itr->price_;
      }
    };

    ss << "Ticker:" << tickerIdToString(ticker_id_) << std::endl;
    {
      auto ask_itr = asks_by_price_;
      auto last_ask_price = std::numeric_limits<Price>::min();
      for (size_t count = 0; ask_itr; ++count) {
        ss << "ASKS L:" << count << " => ";
        auto next_ask_itr = (ask_itr->next_entry_ == asks_by_price_ ? nullptr : ask_itr->next_entry_);
        printer(ss, ask_itr, Side::SELL, last_ask_price, validity_check);
        ask_itr = next_ask_itr;
      }
    }

    ss << std::endl << "                          X" << std::endl << std::endl;

    {
      auto bid_itr = bids_by_price_;
      auto last_bid_price = std::numeric_limits<Price>::max();
      for (size_t count = 0; bid_itr; ++count) {
        ss << "BIDS L:" << count << " => ";
        auto next_bid_itr = (bid_itr->next_entry_ == bids_by_price_ ? nullptr : bid_itr->next_entry_);
        printer(ss, bid_itr, Side::BUY, last_bid_price, validity_check);
        bid_itr = next_bid_itr;
      }
    }

    return ss.str();
  }
}