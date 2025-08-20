#include <csignal>

#include "matcher/matching_engine.h"
#include "market_data/market_data_publisher.h"
#include "order_server/order_server.h"

/// 主要组件，设为全局变量以便信号处理器访问
Common::Logger *logger = nullptr;
Exchange::MatchingEngine *matching_engine = nullptr;
Exchange::MarketDataPublisher *market_data_publisher = nullptr;
Exchange::OrderServer *order_server = nullptr;

/// 外部信号触发时优雅关闭服务器
void signal_handler(int) {
  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);  // 等待10秒以确保资源释放

  // 释放所有组件资源
  delete logger;
  logger = nullptr;
  delete matching_engine;
  matching_engine = nullptr;
  delete market_data_publisher;
  market_data_publisher = nullptr;
  delete order_server;
  order_server = nullptr;

  std::this_thread::sleep_for(10s);  // 等待释放完成

  exit(EXIT_SUCCESS);
}

int main(int, char **) {
  logger = new Common::Logger("exchange_main.log");  // 创建主日志器

  std::signal(SIGINT, signal_handler);  // 注册信号处理器（处理Ctrl+C等中断信号）

  const int sleep_time = 100 * 1000;  // 主循环休眠时间（微秒）

  // 无锁队列，用于订单服务器与匹配引擎、匹配引擎与市场数据发布器之间的通信
  Exchange::ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

  std::string time_str;

  // 启动匹配引擎
  logger->log("%:% %() % 启动匹配引擎...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  matching_engine = new Exchange::MatchingEngine(&client_requests, &client_responses, &market_updates);
  matching_engine->start();

  // 市场数据发布器配置
  const std::string mkt_pub_iface = "lo";
  const std::string snap_pub_ip = "233.252.14.1", inc_pub_ip = "233.252.14.3";
  const int snap_pub_port = 20000, inc_pub_port = 20001;

  // 启动市场数据发布器
  logger->log("%:% %() % 启动市场数据发布器...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  market_data_publisher = new Exchange::MarketDataPublisher(&market_updates, mkt_pub_iface, snap_pub_ip, snap_pub_port, inc_pub_ip, inc_pub_port);
  market_data_publisher->start();

  // 订单服务器配置
  const std::string order_gw_iface = "lo";
  const int order_gw_port = 12345;

  // 启动订单服务器
  logger->log("%:% %() % 启动订单服务器...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  order_server = new Exchange::OrderServer(&client_requests, &client_responses, order_gw_iface, order_gw_port);
  order_server->start();

  // 主循环：持续运行并定期打印日志
  while (true) {
    logger->log("%:% %() % 休眠几毫秒..\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
    usleep(sleep_time * 1000);  // 休眠指定时间
  }
}