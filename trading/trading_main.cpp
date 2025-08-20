#include <csignal>

#include "strategy/trade_engine.h"
#include "order_gw/order_gateway.h"
#include "market_data/market_data_consumer.h"

#include "common/logging.h"

/// 主要组件
Common::Logger *logger = nullptr;
Trading::TradeEngine *trade_engine = nullptr;
Trading::MarketDataConsumer *market_data_consumer = nullptr;
Trading::OrderGateway *order_gateway = nullptr;

/// 程序入口：./trading_main 客户端ID 算法类型 [股票1参数(5个)] [股票2参数(5个)] ...
int main(int argc, char **argv) {
  if(argc < 3) {
    FATAL("使用方法: trading_main 客户端ID 算法类型 [股票1的成交量阈值 价格阈值 最大订单量 最大持仓 最大亏损] [股票2的...参数] ...");
  }

  const Common::ClientId client_id = atoi(argv[1]);
  srand(client_id);  // 以客户端ID为随机数种子

  const auto algo_type = stringToAlgoType(argv[2]);  // 转换算法类型字符串为枚举值

  logger = new Common::Logger("trading_main_" + std::to_string(client_id) + ".log");  // 创建日志器

  const int sleep_time = 20 * 1000;  // 操作间隔时间（微秒）

  // 无锁队列，用于订单网关与交易引擎、市场数据消费者与交易引擎之间的通信
  Exchange::ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

  std::string time_str;

  TradeEngineCfgHashMap ticker_cfg;  // 股票配置哈希映射

  // 从命令行参数解析并初始化股票配置
  // 参数格式：[股票1的成交量阈值 价格阈值 最大订单量 最大持仓 最大亏损] [股票2的...参数] ...
  size_t next_ticker_id = 0;
  for (int i = 3; i < argc; i += 5, ++next_ticker_id) {
    ticker_cfg.at(next_ticker_id) = {
      static_cast<Qty>(std::atoi(argv[i])),          // 成交量阈值
      std::atof(argv[i + 1]),                        // 价格阈值
      {
        static_cast<Qty>(std::atoi(argv[i + 2])),    // 最大订单量
        static_cast<Qty>(std::atoi(argv[i + 3])),    // 最大持仓
        std::atof(argv[i + 4])                       // 最大亏损
      }
    };
  }

  // 启动交易引擎
  logger->log("%:% %() % 启动交易引擎...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  trade_engine = new Trading::TradeEngine(
    client_id, 
    algo_type,
    ticker_cfg,
    &client_requests,
    &client_responses,
    &market_updates
  );
  trade_engine->start();

  // 订单网关配置
  const std::string order_gw_ip = "127.0.0.1";
  const std::string order_gw_iface = "lo";
  const int order_gw_port = 12345;

  // 启动订单网关
  logger->log("%:% %() % 启动订单网关...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  order_gateway = new Trading::OrderGateway(
    client_id, 
    &client_requests, 
    &client_responses, 
    order_gw_ip, 
    order_gw_iface, 
    order_gw_port
  );
  order_gateway->start();

  // 市场数据配置
  const std::string mkt_data_iface = "lo";
  const std::string snapshot_ip = "233.252.14.1";
  const int snapshot_port = 20000;
  const std::string incremental_ip = "233.252.14.3";
  const int incremental_port = 20001;

  // 启动市场数据消费者
  logger->log("%:% %() % 启动市场数据消费者...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  market_data_consumer = new Trading::MarketDataConsumer(
    client_id, 
    &market_updates, 
    mkt_data_iface, 
    snapshot_ip, 
    snapshot_port, 
    incremental_ip, 
    incremental_port
  );
  market_data_consumer->start();

  // 初始化等待时间（10秒）
  usleep(10 * 1000 * 1000);

  trade_engine->initLastEventTime();  // 初始化最后事件时间

  // 随机交易算法实现：生成随机订单并随机取消部分订单
  if (algo_type == AlgoType::RANDOM) {
    Common::OrderId order_id = client_id * 1000;  // 基础订单ID
    std::vector<Exchange::MEClientRequest> client_requests_vec;  // 存储已发送的订单请求
    std::array<Price, ME_MAX_TICKERS> ticker_base_price;  // 各股票基准价格

    // 初始化各股票基准价格
    for (size_t i = 0; i < ME_MAX_TICKERS; ++i)
      ticker_base_price[i] = (rand() % 100) + 100;

    // 生成10000个随机订单
    for (size_t i = 0; i < 10000; ++i) {
      const Common::TickerId ticker_id = rand() % Common::ME_MAX_TICKERS;  // 随机股票
      const Price price = ticker_base_price[ticker_id] + (rand() % 10) + 1;  // 随机价格（基于基准价）
      const Qty qty = 1 + (rand() % 100) + 1;  // 随机数量
      const Side side = (rand() % 2 ? Common::Side::BUY : Common::Side::SELL);  // 随机买卖方向

      // 创建新订单请求
      Exchange::MEClientRequest new_request{
        Exchange::ClientRequestType::NEW, 
        client_id, 
        ticker_id, 
        order_id++, 
        side,
        price, 
        qty
      };
      trade_engine->sendClientRequest(&new_request);  // 发送新订单
      usleep(sleep_time);  // 等待一段时间

      client_requests_vec.push_back(new_request);  // 保存订单请求

      // 随机选择一个已发送的订单进行取消
      const auto cxl_index = rand() % client_requests_vec.size();
      auto cxl_request = client_requests_vec[cxl_index];
      cxl_request.type_ = Exchange::ClientRequestType::CANCEL;  // 修改为取消请求
      trade_engine->sendClientRequest(&cxl_request);  // 发送取消请求
      usleep(sleep_time);  // 等待一段时间

      // 如果60秒内无事件，提前停止
      if (trade_engine->silentSeconds() >= 60) {
        logger->log("%:% %() % 因60秒无活动，提前停止...\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str), trade_engine->silentSeconds());

        break;
      }
    }
  }

  // 等待60秒无活动后停止
  while (trade_engine->silentSeconds() < 60) {
    logger->log("%:% %() % 等待无活动状态，已静默%秒...\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str), trade_engine->silentSeconds());

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(30s);  // 每30秒检查一次
  }

  // 停止各组件
  trade_engine->stop();
  market_data_consumer->stop();
  order_gateway->stop();

  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);  // 等待组件停止

  // 释放资源
  delete logger;
  logger = nullptr;
  delete trade_engine;
  trade_engine = nullptr;
  delete market_data_consumer;
  market_data_consumer = nullptr;
  delete order_gateway;
  order_gateway = nullptr;

  std::this_thread::sleep_for(10s);  // 等待资源释放完成

  exit(EXIT_SUCCESS);
}