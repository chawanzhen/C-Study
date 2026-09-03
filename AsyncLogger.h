#include <vector>
#include <fstream>
#include <mutex>
#include <chrono>
#include <string>
#include <errno.h>
#include <thread>
#include <condition_variable>
#include <iostream>

class AsyncLogger {
public:
	AsyncLogger() {
		current_buffer.reserve(1024 * 1024);//分配1MB的内存
		next_buffer.reserve(1024 * 1024);

		//启动线程
		log_thread = std::thread(&AsyncLogger::work_thread,this);
	}

	~AsyncLogger() {
		_running = false;
		cv.notify_one();//退出时唤醒唯一的wait
		if (log_thread.joinable()) log_thread.join();
		current_buffer.clear();
		next_buffer.clear();
	}

	void append(const std::string& msg) {
		std::unique_lock<std::mutex> lock(_mutex);

		if (current_buffer.size() >= 1024 * 900) {//如果发现当前超过了900kb，唤醒写入
			cv.notify_one();
		}

		if (current_buffer.size() + msg.size() >= 1024 * 1024) {//如果发现当前+要进入的大于了额定内存，暂放入next_buffer中
			next_buffer.insert(next_buffer.end(), msg.begin(), msg.end());
			next_buffer.push_back('\n');
			cv.notify_one();
			return;
		}
		current_buffer.insert(current_buffer.end(), msg.begin(), msg.end());
		current_buffer.push_back('\n');
	}

private:
	void work_thread() {
		std::fstream file("log.log", std::ios::app);

		if (!file) {
			perror("文件打开失败\n");
			return;
		}
		
		std::vector<char>write_buffer;//要写入磁盘的暂存区
		while (_running) {

			{
				std::unique_lock<std::mutex> lock(_mutex);

				if (current_buffer.empty()) {
					cv.wait_for(lock, std::chrono::seconds(3), [this]() {
						return !_running || !current_buffer.empty(); });//等待有数据
				}

				if (!_running && current_buffer.empty() && next_buffer.empty()) break;

				if (next_buffer.empty()) {//对应append的两种情况
					next_buffer.swap(current_buffer);
					write_buffer.swap(next_buffer);
				}
				else {
					write_buffer.swap(current_buffer);
					write_buffer.insert(write_buffer.end(), next_buffer.begin(), next_buffer.end());
					next_buffer.clear();
				}
			}

			if (!write_buffer.empty()) {
				file.write(write_buffer.data(), write_buffer.size());
				file.flush();
				write_buffer.clear();
			}

		}
		file.close();//关闭
	}

private:
	std::vector<char>current_buffer;
	std::vector<char>next_buffer;

	std::mutex _mutex;
	std::thread log_thread;
	std::condition_variable cv;//条件变量
	bool _running = true;
};
#pragma once
