#include <vector>
#include <string>
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")

namespace Parser {

	//状态
	enum parseState
	{
		ReadMagic,
		ReadLength,
		ReadObject
	};

	class frameParse {
	public:
		frameParse() {};
		frameParse(const std::string ma) : magic(ma) {};

		//解析接受的信息
		std::vector<uint8_t> toParser(const char* body,uint16_t msg_length) {
			buffer.insert(buffer.end(), body, body + msg_length);//压入缓冲区

			while (!buffer.empty()) {
				if (state == ReadMagic) {
					if (buffer.size() < 4) return {};
					if (memcmp(buffer.data(), magic.data(), 4) != 0) {
						buffer.erase(buffer.begin());//如果发现魔数与定义不同，直接删除第一个读第二个
						continue;
					}
					state = ReadLength;
					buffer.erase(buffer.begin(), buffer.begin() + 4);
				}
				
				if (state == ReadLength) {
					if (buffer.size() < 4) return {};
					memcpy(&length, buffer.data(), 4);//读取数据大小
					length = ntohl(length);//网络序转化成主机序
					buffer.erase(buffer.begin(), buffer.begin() + 4);

					if (length > 10 * 1024 * 1024) {//防止过大信息传输
						Reset();
						return {};
					}

					state = ReadObject;
				}

				if (state == ReadObject) {
					if (buffer.size() < length) return {};//发现没有接收完全则等待发送完全再接收
					std::vector<uint8_t>protobuf;
					protobuf.insert(protobuf.end(),buffer.begin(), buffer.begin()+ length);
					buffer.erase(buffer.begin(), buffer.begin()+length);
					Reset();//重设状态
					return protobuf;
				}

			}
		}

	private:
		//重置状态
		void Reset() {  state = ReadMagic; length = 0; }

		std::vector<uint8_t>buffer;//缓存区
		parseState state = ReadMagic;//读取状态

		std::string magic = "BHNP";//自定义魔数
		uint32_t length = 0;//长度
	};
}