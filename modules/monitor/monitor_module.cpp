#include "../../include/framework.h"
#include "../../core/include/symbol_manager.h" // For getting ID
#include "ring_buffer.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <zmq.h>
#include <iconv.h>

// WebSocket & JSON
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Helper: GBK -> UTF-8
std::string gbk_to_utf8(const std::string& str) {
    if (str.empty()) return "";
    
    iconv_t cd = iconv_open("UTF-8", "GB18030");
    if (cd == (iconv_t)-1) return str; // Failed

    size_t in_len = str.size();
    size_t out_len = in_len * 3 + 1; // Max expansion
    std::vector<char> out_buf(out_len);
    
    char* in_ptr = const_cast<char*>(str.data());
    char* out_ptr = out_buf.data();
    
    if (iconv(cd, &in_ptr, &in_len, &out_ptr, &out_len) == (size_t)-1) {
        iconv_close(cd);
        return str; // Fallback to raw if failed
    }
    
    iconv_close(cd);
    return std::string(out_buf.data());
}

struct MonitorEvent {
    EventType type;
    union Payload {
        TickRecord md;
        OrderRtn rtn;
        TradeRtn trade;
        PositionDetail pos;
        AccountDetail acc;
    } data;
};

class MonitorModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;

        if (config.count("pub_addr")) pub_addr_ = config.at("pub_addr");
        else pub_addr_ = "tcp://*:5555";

        int ws_port = 8888;
        if (config.count("ws_port")) ws_port = std::stoi(config.at("ws_port"));

        if (config.count("debug")) {
            std::string val = config.at("debug");
            debug_ = (val == "true" || val == "1");
        }
        if (config.count("query_interval")) query_interval_ = std::stoi(config.at("query_interval"));

        std::cout << "[Monitor] 初始化. ZMQ 地址: " << pub_addr_ << ", WS 端口: " << ws_port
                  << ", 查询间隔: " << query_interval_ << "s" << std::endl;

        ws_server_ = std::make_unique<ix::WebSocketServer>(ws_port, "0.0.0.0");
        ws_server_->setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> webSocketPtr,
                   std::shared_ptr<ix::ConnectionState> connectionState) {
                auto webSocket = webSocketPtr.lock();
                if (!webSocket) return;
                webSocket->setOnMessageCallback(
                    [this, webSocket](const ix::WebSocketMessagePtr& msg) {
                        if (msg->type == ix::WebSocketMessageType::Message) {
                            this->handleClientMessage(msg->str);
                        } else if (msg->type == ix::WebSocketMessageType::Open) {
                            if (debug_) std::cout << "[Monitor] WS 客户端已连接，发送持仓快照..." << std::endl;
                            this->sendPositionSnapshot(webSocket);
                        }
                    }
                );
            }
        );

        // 订阅事件 (生产者)
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            MonitorEvent evt;
            evt.type = EVENT_MARKET_DATA;
            std::memcpy(&evt.data.md, d, sizeof(TickRecord));
            queue_.push(evt);
        });

        bus_->subscribe(EVENT_RTN_ORDER, [this](void* d) {
            MonitorEvent evt;
            evt.type = EVENT_RTN_ORDER;
            std::memcpy(&evt.data.rtn, d, sizeof(OrderRtn));
            queue_.push(evt); 
        });

        bus_->subscribe(EVENT_RTN_TRADE, [this](void* d) {
            MonitorEvent evt;
            evt.type = EVENT_RTN_TRADE;
            std::memcpy(&evt.data.trade, d, sizeof(TradeRtn));
            queue_.push(evt);
        });

        bus_->subscribe(EVENT_ACC_UPDATE, [this](void* d) {
            MonitorEvent evt;
            evt.type = EVENT_ACC_UPDATE;
            std::memcpy(&(evt.data.acc), d, sizeof(AccountDetail));
            queue_.push(evt);
        });

        bus_->subscribe(EVENT_POS_UPDATE, [this](void* d) {
            PositionDetail* p = static_cast<PositionDetail*>(d);
            {
                std::lock_guard<std::mutex> lock(pos_mtx_);
                pos_cache_[p->symbol_id] = *p; // 还原回简单的缓存逻辑
            }

            MonitorEvent evt;
            evt.type = EVENT_POS_UPDATE;
            std::memcpy(&evt.data.pos, p, sizeof(PositionDetail));
            queue_.push(evt);
        });
    }

    void start() override {
        running_ = true;
        
        // 启动后立即触发一次持仓查询
        // 资金查询将由 queryLoop 在间隔后触发，避免并发冲突
        bus_->publish(EVENT_QRY_POS, nullptr);

        // Start ZMQ/Broadcasting Thread
        worker_ = std::thread(&MonitorModule::io_loop, this);
        
        // Start Active Query Thread
        if (query_interval_ > 0) {
            query_worker_ = std::thread(&MonitorModule::queryLoop, this);
        }
        
        // Start WS Server (Non-blocking)
        auto res = ws_server_->listen();
        if (!res.first) {
            std::cerr << "[Monitor] WS Listen Failed: " << res.second << std::endl;
        } else {
            ws_server_->start();
            std::cout << "[Monitor] WS Server listening..." << std::endl;
        }
    }

    void stop() override {
        running_ = false;
        if (ws_server_) ws_server_->stop();
        if (query_worker_.joinable()) query_worker_.join();
        if (worker_.joinable()) worker_.join();
    }

private:
    void sendPositionSnapshot(std::shared_ptr<ix::WebSocket> client) {
        std::string json_str = buildSnapshotJson();
        if (json_str.empty()) return;
        client->send(json_str);
    }

    std::string buildSnapshotJson() {
        std::lock_guard<std::mutex> lock(pos_mtx_);
        if (pos_cache_.empty()) return "";

        json root;
        root["type"] = "pos_snapshot";
        root["data"] = json::array();

        for (const auto& kv : pos_cache_) {
            const auto& pos = kv.second;
            json j;
            j["symbol"] = pos.symbol;
            j["symbol_id"] = pos.symbol_id;
            j["long_td"] = pos.long_td;
            j["long_yd"] = pos.long_yd;
            j["long_total"] = pos.long_td + pos.long_yd;
            j["long_price"] = pos.long_avg_price;
            
            j["short_td"] = pos.short_td;
            j["short_yd"] = pos.short_yd;
            j["short_total"] = pos.short_td + pos.short_yd;
            j["short_price"] = pos.short_avg_price;
            
            j["pnl"] = pos.net_pnl;
            root["data"].push_back(j);
        }
        return root.dump();
    }

    // 处理客户端发来的指令 (在 WS 线程池中执行)
    void handleClientMessage(const std::string& msg) {
        if (debug_) std::cout << "[Monitor] 收到指令: " << msg << std::endl;
        try {
            auto j = json::parse(msg);
            std::string action = j.value("action", "");

            if (action == "order") {
                OrderReq req;
                std::string symbol = j.value("symbol", "");
                
                strncpy(req.symbol, symbol.c_str(), 31);
                req.symbol_id = SymbolManager::instance().get_id(req.symbol);
                
                std::string dir = j.value("direction", "B");
                req.direction = dir[0];
                
                std::string off = j.value("offset", "O");
                req.offset_flag = off[0];
                
                req.price = j.value("price", 0.0);
                req.volume = j.value("volume", 1);

                if (debug_) {
                    std::cout << "[Monitor] 远程下单: " << req.symbol << " " 
                              << req.direction << " @ " << req.price << std::endl;
                }

                bus_->publish(EVENT_ORDER_SEND, &req);
            } else if (action == "cancel") {
                CancelReq req;
                std::memset(&req, 0, sizeof(req));
                std::string symbol = j.value("symbol", "");
                std::string order_ref = j.value("order_ref", "");
                
                strncpy(req.symbol, symbol.c_str(), 31);
                strncpy(req.order_ref, order_ref.c_str(), 12);
                
                if (debug_) {
                    std::cout << "[Monitor] 远程撤单: " << req.symbol << " Ref=" << req.order_ref << std::endl;
                }
                bus_->publish(EVENT_CANCEL_REQ, &req);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Monitor] JSON 解析错误: " << e.what() << std::endl;
        }
    }

    void queryLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(query_interval_));
            bus_->publish(EVENT_QRY_POS, nullptr);
            
            // CTP 限制查询频率通常为 1次/秒，这里间隔 1.1s 确保安全
            std::this_thread::sleep_for(std::chrono::milliseconds(1100)); 
            
            bus_->publish(EVENT_QRY_ACC, nullptr);
            if (debug_) std::cout << "[Monitor] 发起持仓和资金查询..." << std::endl;
        }
    }

    void io_loop() {
        void* context = zmq_ctx_new();
        void* publisher = zmq_socket(context, ZMQ_PUB);
        zmq_bind(publisher, pub_addr_.c_str());

        MonitorEvent evt;
        auto last_flush = std::chrono::steady_clock::now();

        while (running_) {
            bool has_event = false;
            // 批量处理队列中的事件，避免频繁锁/IO
            while (queue_.pop(evt)) {
                has_event = true;
                json j;
                
                if (evt.type == EVENT_MARKET_DATA) {
                    j["type"] = "tick";
                    j["symbol"] = evt.data.md.symbol;
                    j["symbol_id"] = evt.data.md.symbol_id;
                    j["price"] = evt.data.md.last_price;
                    j["volume"] = evt.data.md.volume;
                    j["time"] = evt.data.md.update_time;
                } 
                else if (evt.type == EVENT_RTN_ORDER) {
                    j["type"] = "rtn";
                    j["order_ref"] = evt.data.rtn.order_ref;
                    j["symbol"] = evt.data.rtn.symbol;
                    j["direction"] = std::string(1, evt.data.rtn.direction);
                    j["offset"] = std::string(1, evt.data.rtn.offset_flag);
                    j["price"] = evt.data.rtn.limit_price;
                    j["vol_total"] = evt.data.rtn.volume_total;
                    j["vol_traded"] = evt.data.rtn.volume_traded;
                    j["status"] = std::string(1, evt.data.rtn.status);
                    // CTP Msg is GBK, convert to UTF-8 for JSON
                    j["msg"] = gbk_to_utf8(evt.data.rtn.status_msg);
                }
                else if (evt.type == EVENT_RTN_TRADE) {
                    j["type"] = "trade";
                    j["order_ref"] = evt.data.trade.order_ref;
                    j["trade_id"] = evt.data.trade.trade_id;
                    j["symbol"] = evt.data.trade.symbol;
                    j["direction"] = std::string(1, evt.data.trade.direction);
                    j["offset"] = std::string(1, evt.data.trade.offset_flag);
                    j["price"] = evt.data.trade.price;
                    j["volume"] = evt.data.trade.volume;
                }
                else if (evt.type == EVENT_ACC_UPDATE) {
                    j["type"] = "account";
                    j["account_id"] = evt.data.acc.account_id;
                    j["balance"] = evt.data.acc.balance;
                    j["available"] = evt.data.acc.available;
                    j["margin"] = evt.data.acc.margin;
                    j["pnl"] = evt.data.acc.close_pnl + evt.data.acc.position_pnl;
                }
                else if (evt.type == EVENT_POS_UPDATE) {
                    // 标记持仓脏数据，稍后统一推送快照
                    pos_dirty = true;
                    continue; // 跳过单条推送
                }

                if (!j.empty()) {
                    // 使用 ignore_errors 标志进行 dump
                    std::string json_str = j.dump(-1, ' ', false, json::error_handler_t::replace);

                    // 1. ZMQ 广播
                    zmq_send(publisher, json_str.c_str(), json_str.size(), 0);

                    // 2. WebSocket 广播 (文本)
                    if (ws_server_) {
                        for (auto& client : ws_server_->getClients()) {
                            client->send(json_str);
                        }
                    }
                    
                    if (debug_) {
                        std::cout << "[Monitor] 广播数据: " << json_str << std::endl;
                    }
                }
            }

            // 检查是否需要推送持仓快照 (Debounce: 500ms)
            auto now = std::chrono::steady_clock::now();
            if (pos_dirty && (now - last_flush > std::chrono::milliseconds(500))) {
                std::string snapshot = buildSnapshotJson();
                if (!snapshot.empty()) {
                    // ZMQ
                    zmq_send(publisher, snapshot.c_str(), snapshot.size(), 0);
                    // WS
                    if (ws_server_) {
                        for (auto& client : ws_server_->getClients()) {
                            client->send(snapshot);
                        }
                    }
                    if (debug_) std::cout << "[Monitor] 推送持仓快照 (Batch)" << std::endl;
                }
                pos_dirty = false;
                last_flush = now;
            }

            if (!has_event) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        zmq_close(publisher);
        zmq_ctx_destroy(context);
    }

    EventBus* bus_;
    std::string pub_addr_;
    bool debug_ = false; // 调试标志
    int query_interval_ = 5; // 默认 5秒
    std::unique_ptr<ix::WebSocketServer> ws_server_;
    
    // Position Cache for initial snapshots
    std::unordered_map<uint64_t, PositionDetail> pos_cache_;
    std::mutex pos_mtx_;
    std::atomic<bool> pos_dirty{false};

    // Internal queue for decoupling bus and network IO
    RingBuffer<MonitorEvent, 4096> queue_;
    std::thread worker_;
    std::thread query_worker_;
    std::atomic<bool> running_{false};
};

EXPORT_MODULE(MonitorModule)
