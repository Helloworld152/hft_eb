#include "../../include/framework.h"
#include "ring_buffer.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <zmq.h>
#include <iostream>

// 引入系统安装的 RapidJSON
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

struct MonitorEvent {
    EventType type;
    union Payload {
        TickRecord md;
        OrderRtn rtn;
        PositionDetail pos;
    } data;
};

class MonitorModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        if (config.count("pub_addr")) {
            pub_addr_ = config.at("pub_addr");
        } else {
            pub_addr_ = "tcp://*:5555";
        }

        std::cout << "[Monitor] 初始化完成。发布地址: " << pub_addr_ << std::endl;

        // 订阅事件并压入队列（生产者：极速 memcpy）
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

        bus_->subscribe(EVENT_POS_UPDATE, [this](void* d) {
            MonitorEvent evt;
            evt.type = EVENT_POS_UPDATE;
            std::memcpy(&evt.data.pos, d, sizeof(PositionDetail));
            queue_.push(evt);
        });
    }

    void start() override {
        running_ = true;
        worker_ = std::thread(&MonitorModule::io_loop, this);
    }

    void stop() override {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void io_loop() {
        void* context = zmq_ctx_new();
        void* publisher = zmq_socket(context, ZMQ_PUB);
        zmq_bind(publisher, pub_addr_.c_str());

        std::cout << "[Monitor] 后台序列化线程已启动。" << std::endl;

        MonitorEvent evt;
        while (running_) {
            if (queue_.pop(evt)) {
                rapidjson::StringBuffer sb;
                rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
                
                writer.StartObject();
                
                if (evt.type == EVENT_MARKET_DATA) {
                    writer.Key("type"); writer.String("MARKET_DATA");
                    writer.Key("data");
                    writer.StartObject();
                    writer.Key("symbol"); writer.String(evt.data.md.symbol);
                    writer.Key("last_price"); writer.Double(evt.data.md.last_price);
                    writer.Key("volume"); writer.Int(evt.data.md.volume);
                    writer.EndObject();
                } 
                else if (evt.type == EVENT_RTN_ORDER) {
                    writer.Key("type"); writer.String("ORDER_RTN");
                    writer.Key("data");
                    writer.StartObject();
                    writer.Key("order_ref"); writer.String(evt.data.rtn.order_ref);
                    writer.Key("symbol"); writer.String(evt.data.rtn.symbol);
                    writer.Key("status"); writer.String(std::string(1, evt.data.rtn.status).c_str());
                    writer.Key("msg"); writer.String(evt.data.rtn.status_msg);
                    writer.EndObject();
                }
                else if (evt.type == EVENT_POS_UPDATE) {
                    writer.Key("type"); writer.String("POS_UPDATE");
                    writer.Key("data");
                    writer.StartObject();
                    writer.Key("symbol"); writer.String(evt.data.pos.symbol);
                    writer.Key("long_td"); writer.Int(evt.data.pos.long_td);
                    writer.Key("long_yd"); writer.Int(evt.data.pos.long_yd);
                    writer.Key("short_td"); writer.Int(evt.data.pos.short_td);
                    writer.Key("short_yd"); writer.Int(evt.data.pos.short_yd);
                    writer.EndObject();
                }

                writer.EndObject();

                // 发送 JSON 字符串
                zmq_send(publisher, sb.GetString(), sb.GetSize(), 0);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        zmq_close(publisher);
        zmq_ctx_destroy(context);
    }

    EventBus* bus_;
    std::string pub_addr_;
    RingBuffer<MonitorEvent, 1024> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

EXPORT_MODULE(MonitorModule)
