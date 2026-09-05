#ifndef SHELL_WORK_H
#define SHELL_WORK_H

#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <condition_variable>
#include <mutex>
#include <string>

class Shell_Work{
public:
    Shell_Work(){
        bool ok = startShell();
        if(!ok){
            perror("启动失败\n");
            return ;
        }
    }
    ~Shell_Work(){
        _running = false;
        _shell_cv.notify_all();

        stopShell();
    }

    bool startShell(){
        int pin[2],pout[2];
        if(pipe(pin)==-1 || pipe(pout)==-1){
            perror("创建shell管道失败\n");
            return false;
        }

        int pid = fork();
        if(pid == -1){
            perror("fork失败\n");
            return false;
        }

        if(pid == 0){
            close(pin[1]);close(pout[0]);
            dup2(pin[0],STDIN_FILENO);//把标准输入指向pin[0]
            dup2(pout[1],STDOUT_FILENO);
            dup2(pout[1],STDERR_FILENO);//把标准输出和错误指向pout[2];
            execl("/bin/sh","sh",(char*)nullptr);//把子程序改写为持久化shell;
            _exit(127);
        }

        _shell_pid = pid;
        close(pin[0]);close(pout[1]);//关闭输入的读端和输出的写端;
        _shell_read = pout[0];
        _shell_write = pin[1];

        if(_shell_thread.joinable()) _shell_thread.join();
        _shell_thread = std::thread(&Shell_Work::shellRead,this);//启动线程
    }

    std::string doShell(const std::string& data){
        std::string full_cmd = data+"; echo __END__$?\n";
        _shell_done = false;
        if(write(_shell_write,full_cmd.c_str(),full_cmd.size())<0){
            perror("shell失败，尝试重启\n");
            stopShell();
            startShell();
            if(_shell_write == -1 || write(_shell_write,full_cmd.c_str(),full_cmd.size())<0){
                perror("重启失败，退出\n");
                return std::string();
            }
        }

        std::string output;
        {
            std::unique_lock<std::mutex>lk(_shell_mutex);
            //shell 没有完成或者运行结束了
            bool ok = _shell_cv.wait_for(lk,std::chrono::seconds(10),[this](){
                return !_running || _shell_done;
            });
            if(!ok || !_shell_done){
                lk.unlock();
                perror("命令执行超时,尝试重启\n");
                stopShell();
                startShell();
                return std::string();
            }
            output = std::move(_shell_buffer);
            _shell_buffer.clear();
            return output;
        }
        return true;
    }

private:
    void shellRead(){
        char buf[256];
        while(_running && _shell_read!=-1){
            ssize_t n = read(_shell_read,buf,sizeof(buf)-1);
            if(n<=0) break;
            buf[n]='\0';

            std::lock_guard<std::mutex>lk(_shell_mutex);
            _shell_buffer+=buf;

            auto pos = _shell_buffer.rfind("__END__");//倒叙寻找结束标识__END__
            if(pos !=std::string::npos){
                size_t start_pos  = pos+7;
                if(start_pos < _shell_buffer.size()){
                    //_shell_exit = std::stoi(&_shell_buffer[start_pos]);//获得运行结果状态码
                }
                _shell_buffer.resize(pos);
                _shell_done = true;
                _shell_cv.notify_one();
            }
        }

        {
            std::lock_guard<std::mutex> lk(_shell_mutex);
            _shell_done = true;
            _shell_cv.notify_one();
        }
    }

    void stopShell(){
        if(_shell_pid >0) kill(_shell_pid,SIGTERM);
        _shell_pid = -1;
        if(_shell_read!=-1) {close(_shell_read);_shell_read = -1;}
        if(_shell_write!=-1) {close(_shell_write);_shell_write = -1;}
        if(_shell_thread.joinable()) _shell_thread.join();
    }

private:
    std::condition_variable _shell_cv;
    std::mutex _shell_mutex;

    std::string _shell_buffer;
    std::thread _shell_thread;
    int _shell_read = -1;
    int _shell_write = -1;
    int _shell_pid = -1;
    int _shell_exit = -1;

    bool _running = true;

    bool _shell_done = false;
};


#endif // SHELL_WORK_H
