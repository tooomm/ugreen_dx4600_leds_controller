#include "ugreen_leds.h"
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <chrono>
#include <ctime>

#define UGREEN_MAX_LEDS 10

class ugreen_daemon {
    std::mutex pending_lock;
    std::shared_ptr<ugreen_leds_t> leds_controller;
    int probed_leds;
    ugreen_leds_t::led_data_t leds_pending[UGREEN_MAX_LEDS];
    ugreen_leds_t::led_data_t leds_applied[UGREEN_MAX_LEDS];
    bool oneshot_enabled[UGREEN_MAX_LEDS];
    std::time_t oneshot_start_time[UGREEN_MAX_LEDS];

    bool exit_flag;
    std::thread working_thread;

    int sockfd;

public:
    ugreen_daemon(const char *sock_path);
    ~ugreen_daemon() {
        exit_flag = true;
        working_thread.join();
        close(sockfd);
    }

    int accept_and_process();
    void apply_leds();
    void apply_leds(
        ugreen_leds_t::led_type_t led_id, 
        ugreen_leds_t::led_data_t &led_applied,
        ugreen_leds_t::led_data_t &led_pending
    );
};

void ugreen_daemon::apply_leds() {

    ugreen_leds_t::led_data_t leds_pending_copy[UGREEN_MAX_LEDS];

    {
        std::lock_guard<std::mutex> lock(pending_lock);
        std::copy(leds_pending, leds_pending + probed_leds, leds_pending_copy);
    }

    for (int i = 0; i < probed_leds; ++i) {
        apply_leds((ugreen_leds_t::led_type_t)i, leds_applied[i], leds_pending_copy[i]);
    }
}

void ugreen_daemon::apply_leds(
    ugreen_leds_t::led_type_t led_id, 
    ugreen_leds_t::led_data_t &led_applied,
    ugreen_leds_t::led_data_t &led_pending
) {
    auto op_mode = led_pending.op_mode;

    if (oneshot_enabled[(int)led_id]) {
        std::time_t time_diff = std::time(nullptr) - oneshot_start_time[(int)led_id];
        int oneshot_cycle = led_pending.t_on + led_pending.t_off;
        if (time_diff < led_pending.t_on) {
            op_mode = ugreen_leds_t::op_mode_t::on;
        } else if (time_diff < oneshot_cycle) {
            op_mode = ugreen_leds_t::op_mode_t::off;
        } else {
            op_mode = ugreen_leds_t::op_mode_t::on;
        }
    }

    if (op_mode != led_applied.op_mode) {
        switch (led_pending.op_mode) {
            case ugreen_leds_t::op_mode_t::off:
                if (leds_controller->set_onoff(led_id, false) == 0)
                    led_applied.op_mode = led_pending.op_mode;
                return;   // ignore other states, since the led is off
            case ugreen_leds_t::op_mode_t::on:
                if (leds_controller->set_onoff(led_id, true) == 0)
                    led_applied.op_mode = led_pending.op_mode;
                break;
            case ugreen_leds_t::op_mode_t::blink:
                if (leds_controller->set_blink(led_id, led_pending.t_on, led_pending.t_off) == 0) {
                    led_applied.op_mode = led_pending.op_mode;
                    led_applied.t_on = led_pending.t_on;
                    led_applied.t_off = led_pending.t_off;
                }
                break;
            case ugreen_leds_t::op_mode_t::breath:
                if (leds_controller->set_breath(led_id, led_pending.t_on, led_pending.t_off) == 0) {
                    led_applied.op_mode = led_pending.op_mode;
                    led_applied.t_on = led_pending.t_on;
                    led_applied.t_off = led_pending.t_off;
                }
                break;
            default: break;
        }
    }

    if (led_pending.brightness != led_applied.brightness) {
        if (leds_controller->set_brightness(led_id, led_pending.brightness) == 0)
            led_applied.brightness = led_pending.brightness;
    }

    if (led_pending.color_r != led_applied.color_r || 
        led_pending.color_g != led_applied.color_g || 
        led_pending.color_b != led_applied.color_b) {
        if (leds_controller->set_rgb(led_id, led_pending.color_r, led_pending.color_g, led_pending.color_b) == 0) {
            led_applied.color_r = led_pending.color_r;
            led_applied.color_g = led_pending.color_g;
            led_applied.color_b = led_pending.color_b;
        }
    }
}

ugreen_daemon::ugreen_daemon(const char *sock_path) {

    // create a socket
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "Err: fail to create a socket." << std::endl;
        std::exit(-1);
    }

    // bind the socket to a file in /tmp/led-ugreen.socket
    sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sock_path);
    unlink(sock_path);
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Err: fail to bind the socket to " << UGREEN_LED_SOCKET_PATH << std::endl;
        std::exit(-1);
    }

    // listen to the UGREEN_LED_SOCKET_PATH
    if (listen(sockfd, 5) < 0) {
        std::cerr << "Err: fail to listen to the socket." << std::endl;
        std::exit(-1);
    }

    leds_controller = ugreen_leds_t::create_i2c_controller();
    if (leds_controller->start() != 0) {
        std::cerr << "Err: fail to open the I2C device." << std::endl;
        std::cerr << "Please check that (1) you have the root permission; " << std::endl;
        std::cerr << "              and (2) the i2c-dev module is loaded. " << std::endl;
        std::exit(-1);
    }

    // probe the leds
    probed_leds = UGREEN_MAX_LEDS;
    for (int i = 0; i < UGREEN_MAX_LEDS; ++i) {
        leds_pending[i] = leds_applied[i] = leds_controller->get_status((ugreen_leds_t::led_type_t)i);
        if (!leds_applied[i].is_available) {
            probed_leds = i;
            break;
        }
    }

    std::cout << "probed " << probed_leds << " leds." << std::endl;

    // initialize the working thread
    exit_flag = false;
    working_thread = std::thread([this] {
        while (!exit_flag) {
            apply_leds();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

int ugreen_daemon::accept_and_process() {
    // receive the connection from the client
    sockaddr_un client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_sockfd = accept(sockfd, (sockaddr*)&client_addr, &client_addr_len);
    if (client_sockfd < 0) {
        std::cerr << "Err: fail to accept the connection." << std::endl;
        return -1;
    }

    struct socket_guard {
        int sockfd;
        socket_guard(int sockfd) : sockfd(sockfd) {}
        ~socket_guard() {
            // std::cout << "close the socket." << std::endl;
            close(sockfd);
        }
    };

    socket_guard guard(client_sockfd);

    char buffer[256];
    std::stringstream ss;
    while (true) {
        int n = read(client_sockfd, buffer, sizeof(buffer));
        if (n < 0) {
            std::cerr << "Err: fail to read the message." << std::endl;
            return -1;
        }
        buffer[n] = '\0';

        ss << buffer;

        std::string command;
        int led_id;
        ss >> led_id >> command;

        if (led_id < 0 || led_id >= UGREEN_MAX_LEDS) {
            std::cerr << "Err: invalid led id " << led_id  << "." << std::endl;
            return -1;
        }

        // std::cout << "led_id: " << led_id << ", command: " << command << std::endl;

        if (command == "brightness_set") {
            int brightness;
            ss >> brightness;

            std::lock_guard<std::mutex> lock(pending_lock);

            if (brightness == 0) {
                leds_pending[led_id].op_mode = ugreen_leds_t::op_mode_t::off;
            } else {
                if (leds_pending[led_id].op_mode == ugreen_leds_t::op_mode_t::off)
                    leds_pending[led_id].op_mode = ugreen_leds_t::op_mode_t::on;

                leds_pending[led_id].brightness = brightness;
            }

        } else if (command == "color_set") {
            int r, g, b;
            ss >> r >> g >> b;

            if (r != 0 || g != 0 || b != 0) {
                std::lock_guard<std::mutex> lock(pending_lock);

                leds_pending[led_id].color_r = r;
                leds_pending[led_id].color_g = g;
                leds_pending[led_id].color_b = b;
            }
        } else if (command == "on") {
            std::lock_guard<std::mutex> lock(pending_lock);
            leds_pending[led_id].op_mode = ugreen_leds_t::op_mode_t::on;
        } else if (command == "off") {
            std::lock_guard<std::mutex> lock(pending_lock);
            leds_pending[led_id].op_mode = ugreen_leds_t::op_mode_t::off;
        } else if (command == "blink") {
            std::string blink_type;
            int t_on, t_off;
            ss >> blink_type >> t_on >> t_off;

            std::lock_guard<std::mutex> lock(pending_lock);

            if (blink_type == "blink") {
                leds_pending[led_id].op_mode = ugreen_leds_t::op_mode_t::blink;
            } else if(blink_type == "breath") {
                leds_pending[led_id].op_mode = ugreen_leds_t::op_mode_t::breath;
            } else {
                std::cerr << "Err: invalid blink type." << std::endl;
                return -1;
            }

            leds_pending[led_id].t_on = std::min(0x7fff, std::max(50, t_on));
            leds_pending[led_id].t_off = std::min(0x7fff, std::max(50, t_off));
        } else if (command == "oneshot_set") {
            int t_on, t_off;
            ss >> t_on >> t_off;

            std::lock_guard<std::mutex> lock(pending_lock);

            leds_pending[led_id].t_on = std::min(0x7fff, std::max(50, t_on));
            leds_pending[led_id].t_off = std::min(0x7fff, std::max(50, t_off));
            oneshot_enabled[led_id] = true;
        } else if (command == "shot") {

            std::time_t time_diff = std::time(nullptr) - oneshot_start_time[led_id];

            std::lock_guard<std::mutex> lock(pending_lock);

            int oneshot_cycle = leds_pending[led_id].t_on + leds_pending[led_id].t_off;
            if (time_diff > oneshot_cycle || !oneshot_enabled[led_id]) {
                oneshot_start_time[led_id] = std::time(nullptr);
            }
        } else if (command == "status") {
            ugreen_leds_t::led_data_t led_status;

            {
                std::lock_guard<std::mutex> lock(pending_lock);
                led_status = leds_pending[led_id];
            }

            std::stringstream ss;
            ss << (int)(led_id < probed_leds) << " "
               << (int)led_status.op_mode << " " 
               << (int)led_status.brightness << " "
               << (int)led_status.color_r << " " 
               << (int)led_status.color_g << " " 
               << (int)led_status.color_b << " "
               << led_status.t_on << " " 
               << led_status.t_off << "\n";

            send(client_sockfd, ss.str().c_str(), ss.str().size(), 0);
        } else if (command == "exit") {
            // std::cout << "exit the loop." << std::endl;
            break;
        } else {
            std::cerr << "Err: invalid command " << command << "." << std::endl;
            return -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    ugreen_daemon daemon(UGREEN_LED_SOCKET_PATH);
    while(true) daemon.accept_and_process();
    return 0;
}
