#include "order_manager.h"
#include "trade_engine.h"

namespace Trading {
  // 发送具有指定属性的新订单，并更新传入的OMOrder对象
  auto OrderManager::newOrder(OMOrder *order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept -> void {
    // 构造新订单请求
    const Exchange::MEClientRequest new_request{Exchange::ClientRequestType::NEW, trade_engine_->clientId(), ticker_id,
                                                next_order_id_, side, price, qty};
    // 通过交易引擎发送客户端请求
    trade_engine_->sendClientRequest(&new_request);

    // 更新订单对象信息（设置股票代码、订单ID、方向、价格、数量及待新建状态）
    *order = {ticker_id, next_order_id_, side, price, qty, OMOrderState::PENDING_NEW};
    // 递增下一个订单ID（用于下一次新订单）
    ++next_order_id_;

    logger_->log("%:% %() % Sent new order % for %\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 new_request.toString().c_str(), order->toString().c_str());
  }

  // 发送指定订单的取消请求，并更新传入的OMOrder对象
  auto OrderManager::cancelOrder(OMOrder *order) noexcept -> void {
    // 构造取消订单请求
    const Exchange::MEClientRequest cancel_request{Exchange::ClientRequestType::CANCEL, trade_engine_->clientId(),
                                                   order->ticker_id_, order->order_id_, order->side_, order->price_,
                                                   order->qty_};
    // 通过交易引擎发送客户端请求
    trade_engine_->sendClientRequest(&cancel_request);

    // 更新订单状态为待取消
    order->order_state_ = OMOrderState::PENDING_CANCEL;

    logger_->log("%:% %() % Sent cancel % for %\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 cancel_request.toString().c_str(), order->toString().c_str());
  }
}