#include "config/config.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

std::string ExtractString(const std::string& text, const std::string& key) {
    auto pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";

    pos = text.find(':', pos);
    if (pos == std::string::npos) return "";

    ++pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }

    if (pos < text.size() && text[pos] == '"') {
        size_t end = text.find('"', pos + 1);
        if (end != std::string::npos) {
            return text.substr(pos + 1, end - pos - 1);
        }
    }

    return "";
}

int ExtractInt(const std::string& text, const std::string& key) {
    auto pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;

    pos = text.find(':', pos);
    if (pos == std::string::npos) return 0;

    ++pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }

    std::string num;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        num += text[pos];
        ++pos;
    }

    return num.empty() ? 0 : std::stoi(num);
}

}  // namespace

Config load_config(const std::string& filename) {
    std::ifstream file(filename);
    if (!file) {
        throw std::runtime_error("Cannot open config file: " + filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();

    Config cfg;
    cfg.listen_port = static_cast<uint16_t>(ExtractInt(text, "listen_port"));
    cfg.balancer = ExtractString(text, "balancer");
    cfg.health_check_interval_sec = ExtractInt(text, "health_check_interval_sec");
    cfg.health_check_timeout_sec = ExtractInt(text, "health_check_timeout_sec");

    // 缺失或 <= 0 用默认值
    cfg.worker_threads = ExtractInt(text, "worker_threads");
    if (cfg.worker_threads <= 0) cfg.worker_threads = 4;
    cfg.output_buffer_kb = ExtractInt(text, "output_buffer_kb");
    if (cfg.output_buffer_kb <= 0) cfg.output_buffer_kb = 512;

    // 简化 JSON 解析：只识别 {"address":..., "port":...}，不支持嵌套和转义
    auto backends_start = text.find("\"backends\"");
    if (backends_start != std::string::npos) {
        auto arr_start = text.find('[', backends_start);
        auto arr_end = text.find(']', arr_start);

        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string arr = text.substr(arr_start + 1, arr_end - arr_start - 1);
            size_t obj_start = 0;

            while ((obj_start = arr.find('{', obj_start)) != std::string::npos) {
                size_t obj_end = arr.find('}', obj_start);
                if (obj_end == std::string::npos) break;

                std::string obj = arr.substr(obj_start, obj_end - obj_start + 1);

                BackendConfig bc;
                bc.address = ExtractString(obj, "address");
                bc.port = static_cast<uint16_t>(ExtractInt(obj, "port"));
                cfg.backends.push_back(bc);

                obj_start = obj_end + 1;
            }
        }
    }

    return cfg;
}