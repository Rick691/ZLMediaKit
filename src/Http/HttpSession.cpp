﻿/*
 * MIT License
 *
 * Copyright (c) 2016-2019 xiongziliang <771730766@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#if !defined(_WIN32)
#include <dirent.h>
#endif //!defined(_WIN32)

#include <stdio.h>
#include <sys/stat.h>
#include <algorithm>
#include <iomanip>

#include "Common/config.h"
#include "strCoding.h"
#include "HttpSession.h"
#include "Util/File.h"
#include "Util/util.h"
#include "Util/TimeTicker.h"
#include "Util/onceToken.h"
#include "Util/mini.h"
#include "Util/NoticeCenter.h"
#include "Util/base64.h"
#include "Util/SHA1.h"
#include "Rtmp/utils.h"
using namespace toolkit;

namespace mediakit {

static int kSockFlags = SOCKET_DEFAULE_FLAGS | FLAG_MORE;
static int kHlsCookieSecond = 10 * 60;
static const string kCookieName = "ZL_COOKIE";
static const string kCookiePathKey = "kCookiePathKey";
static const string kAccessErrKey = "kAccessErrKey";

string dateStr() {
	char buf[64];
	time_t tt = time(NULL);
	strftime(buf, sizeof buf, "%a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
	return buf;
}
static const char*
get_mime_type(const char* name) {
	const char* dot;
	dot = strrchr(name, '.');
	static HttpSession::KeyValue mapType;
	static onceToken token([&]() {
		mapType.emplace(".html","text/html");
		mapType.emplace(".htm","text/html");
		mapType.emplace(".mp4","video/mp4");
		mapType.emplace(".m3u8","application/vnd.apple.mpegurl");
		mapType.emplace(".jpg","image/jpeg");
		mapType.emplace(".jpeg","image/jpeg");
		mapType.emplace(".gif","image/gif");
		mapType.emplace(".png","image/png");
		mapType.emplace(".ico","image/x-icon");
		mapType.emplace(".css","text/css");
		mapType.emplace(".js","application/javascript");
		mapType.emplace(".au","audio/basic");
		mapType.emplace(".wav","audio/wav");
		mapType.emplace(".avi","video/x-msvideo");
		mapType.emplace(".mov","video/quicktime");
		mapType.emplace(".qt","video/quicktime");
		mapType.emplace(".mpeg","video/mpeg");
		mapType.emplace(".mpe","video/mpeg");
		mapType.emplace(".vrml","model/vrml");
		mapType.emplace(".wrl","model/vrml");
		mapType.emplace(".midi","audio/midi");
		mapType.emplace(".mid","audio/midi");
		mapType.emplace(".mp3","audio/mpeg");
		mapType.emplace(".ogg","application/ogg");
		mapType.emplace(".pac","application/x-ns-proxy-autoconfig");
        mapType.emplace(".flv","video/x-flv");
	}, nullptr);
	if(!dot){
		return "text/plain";
	}
	auto it = mapType.find(dot);
	if (it == mapType.end()) {
		return "text/plain";
	}
	return it->second.data();
}


HttpSession::HttpSession(const Socket::Ptr &pSock) : TcpSession(pSock) {
    TraceP(this);
    GET_CONFIG(uint32_t,keep_alive_sec,Http::kKeepAliveSecond);
    pSock->setSendTimeOutSecond(keep_alive_sec);
	//起始接收buffer缓存设置为4K，节省内存
	pSock->setReadBuffer(std::make_shared<BufferRaw>(4 * 1024));
}

HttpSession::~HttpSession() {
    TraceP(this);
}

int64_t HttpSession::onRecvHeader(const char *header,uint64_t len) {
	typedef void (HttpSession::*HttpCMDHandle)(int64_t &);
	static unordered_map<string, HttpCMDHandle> g_mapCmdIndex;
	static onceToken token([]() {
		g_mapCmdIndex.emplace("GET",&HttpSession::Handle_Req_GET);
		g_mapCmdIndex.emplace("POST",&HttpSession::Handle_Req_POST);
		//chenxiaolei  增加OPTIONS,以便支持web 页面的跨域嗅探请求
		g_mapCmdIndex.emplace("OPTIONS",&HttpSession::Handle_Req_OPTIONS);
	}, nullptr);

	_parser.Parse(header);
	urlDecode(_parser);
	string cmd = _parser.Method();
	auto it = g_mapCmdIndex.find(cmd);
	if (it == g_mapCmdIndex.end()) {
		sendResponse("403 Forbidden", makeHttpHeader(true), "");
        shutdown(SockException(Err_shutdown,StrPrinter << "403 Forbidden:" << cmd));
        return 0;
	}

	//默认后面数据不是content而是header
	int64_t content_len = 0;
	auto &fun = it->second;
    try {
        (this->*fun)(content_len);
    }catch (SockException &ex){
        if(ex){
            shutdown(ex);
        }
    }catch (exception &ex){
        shutdown(SockException(Err_shutdown,ex.what()));
    }

	//清空解析器节省内存
	_parser.Clear();
	//返回content长度
	return content_len;
}

void HttpSession::onRecvContent(const char *data,uint64_t len) {
	if(_contentCallBack){
		if(!_contentCallBack(data,len)){
			_contentCallBack = nullptr;
		}
	}
}

void HttpSession::onRecv(const Buffer::Ptr &pBuf) {
    _ticker.resetTime();
    input(pBuf->data(),pBuf->size());
}

void HttpSession::onError(const SockException& err) {
    if(_ticker.createdTime() < 10 * 1000){
        TraceP(this) << err.what();
    }else{
        WarnP(this) << err.what();
    }

    GET_CONFIG(uint32_t,iFlowThreshold,General::kFlowThreshold);
    if(_ui64TotalBytes > iFlowThreshold * 1024){
        NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport,
										   _mediaInfo,
										   _ui64TotalBytes,
										   _ticker.createdTime()/1000,
										   true,
										   *this);
    }
}

void HttpSession::onManager() {
    GET_CONFIG(uint32_t,keepAliveSec,Http::kKeepAliveSecond);

    if(_ticker.elapsedTime() > keepAliveSec * 1000){
		//1分钟超时
		shutdown(SockException(Err_timeout,"session timeouted"));
	}
}


inline bool HttpSession::checkWebSocket(){
	auto Sec_WebSocket_Key = _parser["Sec-WebSocket-Key"];
	if(Sec_WebSocket_Key.empty()){
		return false;
	}
	auto Sec_WebSocket_Accept = encodeBase64(SHA1::encode_bin(Sec_WebSocket_Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));

	KeyValue headerOut;
	headerOut["Upgrade"] = "websocket";
	headerOut["Connection"] = "Upgrade";
	headerOut["Sec-WebSocket-Accept"] = Sec_WebSocket_Accept;
	if(!_parser["Sec-WebSocket-Protocol"].empty()){
		headerOut["Sec-WebSocket-Protocol"] = _parser["Sec-WebSocket-Protocol"];
	}
	sendResponse("101 Switching Protocols",headerOut,"");
	return true;
}
//http-flv 链接格式:http://vhost-url:port/app/streamid.flv?key1=value1&key2=value2
//如果url(除去?以及后面的参数)后缀是.flv,那么表明该url是一个http-flv直播。
inline bool HttpSession::checkLiveFlvStream(){
	auto pos = strrchr(_parser.Url().data(),'.');
	if(!pos){
		//未找到".flv"后缀
		return false;
	}
	if(strcasecmp(pos,".flv") != 0){
		//未找到".flv"后缀
		return false;
	}

	//这是个.flv的流
    _mediaInfo.parse(string(RTMP_SCHEMA) + "://" + _parser["Host"] + _parser.FullUrl());
	if(_mediaInfo._app.empty() || _mediaInfo._streamid.size() < 5){
	    //url不合法
        return false;
	}
    _mediaInfo._streamid.erase(_mediaInfo._streamid.size() - 4);//去除.flv后缀

    GET_CONFIG(uint32_t,reqCnt,Http::kMaxReqCount);
    bool bClose = (strcasecmp(_parser["Connection"].data(),"close") == 0) || ( ++_iReqCnt > reqCnt);

    weak_ptr<HttpSession> weakSelf = dynamic_pointer_cast<HttpSession>(shared_from_this());
    MediaSource::findAsync(_mediaInfo,weakSelf.lock(), true,[weakSelf,bClose,this](const MediaSource::Ptr &src){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            //本对象已经销毁
            return;
        }
        auto rtmp_src = dynamic_pointer_cast<RtmpMediaSource>(src);
        if(!rtmp_src){
            //未找到该流
            sendNotFound(bClose);
            if(bClose){
                shutdown(SockException(Err_shutdown,"flv stream not found"));
            }
            return;
        }
        //找到流了
        auto onRes = [this,rtmp_src](const string &err){
            bool authSuccess = err.empty();
            if(!authSuccess){
                sendResponse("401 Unauthorized", makeHttpHeader(true,err.size()),err);
                shutdown(SockException(Err_shutdown,StrPrinter << "401 Unauthorized:" << err));
                return ;
            }

            //找到rtmp源，发送http头，负载后续发送
            sendResponse("200 OK", makeHttpHeader(false,0,get_mime_type(".flv")), "");

            //开始发送rtmp负载
            //关闭tcp_nodelay ,优化性能
            SockUtil::setNoDelay(_sock->rawFD(),false);
            (*this) << SocketFlags(kSockFlags);

            try{
                start(getPoller(),rtmp_src);
            }catch (std::exception &ex){
                //该rtmp源不存在
                shutdown(SockException(Err_shutdown,"rtmp mediasource released"));
            }
        };

        weak_ptr<HttpSession> weakSelf = dynamic_pointer_cast<HttpSession>(shared_from_this());
        Broadcast::AuthInvoker invoker = [weakSelf,onRes](const string &err){
            auto strongSelf = weakSelf.lock();
            if(!strongSelf){
                return;
            }
            strongSelf->async([weakSelf,onRes,err](){
                auto strongSelf = weakSelf.lock();
                if(!strongSelf){
                    return;
                }
                onRes(err);
            });
        };
        auto flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPlayed,_mediaInfo,invoker,*this);
        if(!flag){
            //该事件无人监听,默认不鉴权
            onRes("");
        }
    });
    return true;
}

inline bool makeMeun(const string &httpPath,const string &strFullPath, string &strRet) ;

inline static string findIndexFile(const string &dir){
    DIR *pDir;
    dirent *pDirent;
    if ((pDir = opendir(dir.data())) == NULL) {
        return "";
    }
    set<string> setFile;
    while ((pDirent = readdir(pDir)) != NULL) {
        static set<const char *,StrCaseCompare> indexSet = {"index.html","index.htm","index"};
        if(indexSet.find(pDirent->d_name) !=  indexSet.end()){
            closedir(pDir);
            return pDirent->d_name;
        }
    }
    closedir(pDir);
    return "";
}

inline string HttpSession::getClientUid(){
    //如果http客户端不支持cookie，那么我们可以通过url参数来追踪用户
    //如果url参数也没有，那么只能通过ip+端口号来追踪用户
    //追踪用户的目的是为了减少http短链接情况的重复鉴权验证，通过缓存记录鉴权结果，提高性能
    string uid = _parser.Params();
    if(uid.empty()){
        uid = StrPrinter << get_peer_ip() << ":" << get_peer_port();
    }
    return uid;
}


//字符串是否以xx结尾
static inline bool end_of(const string &str, const string &substr){
    auto pos = str.rfind(substr);
    return pos != string::npos && pos == str.size() - substr.size();
};

//拦截hls的播放请求
static inline bool checkHls(BroadcastHttpAccessArgs){
    if(!end_of(args._streamid,("/hls.m3u8"))) {
        //不是hls
        return false;
    }
    //访问的hls.m3u8结尾，我们转换成kBroadcastMediaPlayed事件
    Broadcast::AuthInvoker mediaAuthInvoker = [invoker,path](const string &err){
        //cookie有效期为kHlsCookieSecond
        invoker(err,"",kHlsCookieSecond);
    };

    auto args_copy = args;
    replace(args_copy._streamid,"/hls.m3u8","");
    return NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPlayed,args_copy,mediaAuthInvoker,sender);
}

inline void HttpSession::canAccessPath(const string &path_in,bool is_dir,const function<void(const string &errMsg,const HttpServerCookie::Ptr &cookie)> &callback_in){
    auto path = path_in;
    replace(const_cast<string &>(path),"//","/");

    auto callback = [callback_in,this](const string &errMsg,const HttpServerCookie::Ptr &cookie){
        try {
            callback_in(errMsg,cookie);
        }catch (SockException &ex){
            if(ex){
                shutdown(ex);
            }
        }catch (exception &ex){
            shutdown(SockException(Err_shutdown,ex.what()));
        }
    };

    //获取用户唯一id
    auto uid = getClientUid();
    //先根据http头中的cookie字段获取cookie
    HttpServerCookie::Ptr cookie = HttpCookieManager::Instance().getCookie(kCookieName, _parser.getValues());
    if(!cookie){
        //客户端请求中无cookie,再根据该用户的用户id获取cookie
        cookie = HttpCookieManager::Instance().getCookieByUid(kCookieName, uid);
    }

    if(cookie){
        //找到了cookie，对cookie上锁先
        auto lck = cookie->getLock();
        auto accessErr = (*cookie)[kAccessErrKey];
        if(path.find((*cookie)[kCookiePathKey]) == 0){
            //上次cookie是限定本目录
            if(accessErr.empty()){
                //上次鉴权成功
                callback("", nullptr);
                return;
            }
            //上次鉴权失败，如果url发生变更，那么也重新鉴权
            if (_parser.Params().empty() || _parser.Params() == cookie->getUid()) {
                //url参数未变，那么判断无权限访问
                callback(accessErr.empty() ? "无权限访问该目录" : accessErr, nullptr);
                return;
            }
        }
        //如果url参数变了或者不是限定本目录，那么旧cookie失效，重新鉴权
        HttpCookieManager::Instance().delCookie(cookie);
    }

    //该用户从来未获取过cookie，这个时候我们广播是否允许该用户访问该http目录
    weak_ptr<HttpSession> weakSelf = dynamic_pointer_cast<HttpSession>(shared_from_this());
    HttpAccessPathInvoker accessPathInvoker = [weakSelf,callback,uid,path,is_dir] (const string &errMsg,const string &cookie_path_in, int cookieLifeSecond) {
        HttpServerCookie::Ptr cookie ;
        if(cookieLifeSecond) {
            //本次鉴权设置了有效期，我们把鉴权结果缓存在cookie中
            string cookie_path = cookie_path_in;
            if(cookie_path.empty()){
                //如果未设置鉴权目录，那么我们采用当前目录
                cookie_path = is_dir ? path : path.substr(0,path.rfind("/") + 1);
            }

            cookie = HttpCookieManager::Instance().addCookie(kCookieName, uid, cookieLifeSecond);
            //对cookie上锁
            auto lck = cookie->getLock();
            //记录用户能访问的路径
            (*cookie)[kCookiePathKey] = cookie_path;
            //记录能否访问
            (*cookie)[kAccessErrKey] = errMsg;
        }

        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            //自己已经销毁
            return;
        }
        strongSelf->async([weakSelf,callback,cookie,errMsg]() {
            //切换到自己线程
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                //自己已经销毁
                return;
            }
            callback(errMsg, cookie);
        });
    };

    if(checkHls(_parser,_mediaInfo,path,is_dir,accessPathInvoker,*this)){
        //是hls的播放鉴权,拦截之
        return;
    }

    bool flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastHttpAccess,_parser,_mediaInfo,path,is_dir,accessPathInvoker,*this);
    if(!flag){
        //此事件无人监听，我们默认都有权限访问
        callback("", nullptr);
    }

}

inline void HttpSession::Handle_Req_GET(int64_t &content_len) {
	//先看看是否为WebSocket请求
	if(checkWebSocket()){
		content_len = -1;
		auto parserCopy = _parser;
		_contentCallBack = [this,parserCopy](const char *data,uint64_t len){
			onRecvWebSocketData(parserCopy,data,len);
			//_contentCallBack是可持续的，后面还要处理后续数据
			return true;
		};
		return;
	}

	//先看看该http事件是否被拦截
	if(emitHttpEvent(false)){
		return;
	}

    //再看看是否为http-flv直播请求
	if(checkLiveFlvStream()){
		return;
	}

	//事件未被拦截，则认为是http下载请求
	auto fullUrl = string(HTTP_SCHEMA) + "://" + _parser["Host"] + _parser.FullUrl();
    _mediaInfo.parse(fullUrl);

	/////////////HTTP连接是否需要被关闭////////////////
    GET_CONFIG(uint32_t,reqCnt,Http::kMaxReqCount);
    GET_CONFIG(bool,enableVhost,General::kEnableVhost);
    GET_CONFIG(string,rootPath,Http::kRootPath);
    string strFile = enableVhost ?  rootPath + "/" + _mediaInfo._vhost + _parser.Url() :rootPath + _parser.Url();
    bool bClose = (strcasecmp(_parser["Connection"].data(),"close") == 0) || ( ++_iReqCnt > reqCnt);

    do{
        //访问的是文件夹
        if (strFile.back() == '/' || File::is_dir(strFile.data())) {
            auto indexFile = findIndexFile(strFile);
            if(!indexFile.empty()){
                //发现该文件夹下有index文件
                strFile = strFile + "/" + indexFile;
                _parser.setUrl(_parser.Url() + "/" + indexFile);
                break;
            }
            string strMeun;
            //生成文件夹菜单索引
            if (!makeMeun(_parser.Url(),strFile,strMeun)) {
                //文件夹不存在
                sendNotFound(bClose);
                throw SockException(bClose ? Err_shutdown : Err_success,"close connection after send 404 not found on folder");
            }

            //判断是否有权限访问该目录
            canAccessPath(_parser.Url(),true,[this,bClose,strFile,strMeun](const string &errMsg,const HttpServerCookie::Ptr &cookie){
                if(!errMsg.empty()){
                    const_cast<string &>(strMeun) = errMsg;
                }
                auto headerOut = makeHttpHeader(bClose,strMeun.size());
                if(cookie){
                    headerOut["Set-Cookie"] = cookie->getCookie((*cookie)[kCookiePathKey]);
                }
                sendResponse(errMsg.empty() ? "200 OK" : "401 Unauthorized" , headerOut, strMeun);
                throw SockException(bClose ? Err_shutdown : Err_success,"close connection after access folder");
            });
            return;
        }
    }while(0);

	//访问的是文件
	struct stat tFileStat;
	if (0 != stat(strFile.data(), &tFileStat)) {
		//文件不存在
		sendNotFound(bClose);
        throw SockException(bClose ? Err_shutdown : Err_success,"close connection after send 404 not found on file");
	}
    //文件智能指针，防止退出时未关闭
    std::shared_ptr<FILE> pFilePtr(fopen(strFile.data(), "rb"), [](FILE *pFile) {
        if(pFile){
            fclose(pFile);
        }
    });

	if (!pFilePtr) {
		//打开文件失败
		sendNotFound(bClose);
        throw SockException(bClose ? Err_shutdown : Err_success,"close connection after send 404 not found on open file failed");
	}

	auto parser = _parser;
    //判断是否有权限访问该文件
    canAccessPath(_parser.Url(),false,[this,parser,tFileStat,pFilePtr,bClose,strFile](const string &errMsg,const HttpServerCookie::Ptr &cookie){
        if(!errMsg.empty()){
            auto headerOut = makeHttpHeader(bClose,errMsg.size());
            if(cookie){
                headerOut["Set-Cookie"] = cookie->getCookie((*cookie)[kCookiePathKey]);
            }
            sendResponse("401 Unauthorized" , headerOut, errMsg);
            throw SockException(bClose ? Err_shutdown : Err_success,"close connection after access file failed");
        }

        //判断是不是分节下载
        auto &strRange = parser["Range"];
        int64_t iRangeStart = 0, iRangeEnd = 0;
        iRangeStart = atoll(FindField(strRange.data(), "bytes=", "-").data());
        iRangeEnd = atoll(FindField(strRange.data(), "-", "\r\n").data());
        if (iRangeEnd == 0) {
            iRangeEnd = tFileStat.st_size - 1;
        }
        const char *pcHttpResult = NULL;
        if (strRange.size() == 0) {
            //全部下载
            pcHttpResult = "200 OK";
        } else {
            //分节下载
            pcHttpResult = "206 Partial Content";
            fseek(pFilePtr.get(), iRangeStart, SEEK_SET);
        }
        auto httpHeader =  makeHttpHeader(bClose, iRangeEnd - iRangeStart + 1, get_mime_type(strFile.data()));
        if (strRange.size() != 0) {
            //分节下载返回Content-Range头
            httpHeader.emplace("Content-Range",StrPrinter<<"bytes " << iRangeStart << "-" << iRangeEnd << "/" << tFileStat.st_size<< endl);
        }
        auto Origin = parser["Origin"];
        if(!Origin.empty()){
            httpHeader["Access-Control-Allow-Origin"] = Origin;
            httpHeader["Access-Control-Allow-Credentials"] = "true";
        }

        //先回复HTTP头部分
        sendResponse(pcHttpResult,httpHeader,"");

        if (iRangeEnd - iRangeStart < 0) {
            //文件是空的!
            throw SockException(bClose ? Err_shutdown : Err_success,"close connection after access file");
        }
        //回复Content部分
        std::shared_ptr<int64_t> piLeft(new int64_t(iRangeEnd - iRangeStart + 1));

        GET_CONFIG(uint32_t,sendBufSize,Http::kSendBufSize);

        weak_ptr<HttpSession> weakSelf = dynamic_pointer_cast<HttpSession>(shared_from_this());
        auto onFlush = [pFilePtr,bClose,weakSelf,piLeft]() {
            TimeTicker();
            auto strongSelf = weakSelf.lock();
            while(*piLeft && strongSelf){
                //更新超时定时器
                strongSelf->_ticker.resetTime();
                //从循环池获取一个内存片
                auto sendBuf = strongSelf->obtainBuffer();
                sendBuf->setCapacity(sendBufSize);
                //本次需要读取文件字节数
                int64_t iReq = MIN(sendBufSize,*piLeft);
                //读文件
                int iRead;
                do{
                    iRead = fread(sendBuf->data(), 1, iReq, pFilePtr.get());
                }while(-1 == iRead && UV_EINTR == get_uv_error(false));
                //文件剩余字节数
                *piLeft -= iRead;

                if (iRead < iReq || !*piLeft) {
                    //文件读完
                    if(iRead>0) {
                        sendBuf->setSize(iRead);
                        strongSelf->send(sendBuf);
                    }
                    if(bClose) {
                        strongSelf->shutdown(SockException(Err_shutdown,"read file eof"));
                    }
                    return false;
                }
                //文件还未读完
                sendBuf->setSize(iRead);
                int iSent = strongSelf->send(sendBuf);
                if(iSent == -1) {
                    //套机制销毁
                    return false;
                }
                if(strongSelf->isSocketBusy()){
                    //套接字忙，那么停止继续写
                    return true;
                }
                //继续写套接字
            }
            return false;
        };
        //关闭tcp_nodelay ,优化性能
        SockUtil::setNoDelay(_sock->rawFD(),false);
        //设置MSG_MORE，优化性能
        (*this) << SocketFlags(kSockFlags);

        onFlush();
        _sock->setOnFlush(onFlush);
    });
}

inline bool makeMeun(const string &httpPath,const string &strFullPath, string &strRet) {
	string strPathPrefix(strFullPath);
	string last_dir_name;
	if(strPathPrefix.back() == '/'){
		strPathPrefix.pop_back();
	}else{
		last_dir_name = split(strPathPrefix,"/").back();
	}

	if (!File::is_dir(strPathPrefix.data())) {
		return false;
	}
	stringstream ss;
	ss <<   "<html>\r\n"
			"<head>\r\n"
			"<title>文件索引</title>\r\n"
			"</head>\r\n"
			"<body>\r\n"
			"<h1>文件索引:";

	ss << httpPath;
	ss << "</h1>\r\n";
	if (httpPath != "/") {
		ss << "<li><a href=\"";
		ss << "/";
		ss << "\">";
		ss << "根目录";
		ss << "</a></li>\r\n";

		ss << "<li><a href=\"";
		if(!last_dir_name.empty()){
			ss << "./";
		}else{
			ss << "../";
		}
		ss << "\">";
		ss << "上级目录";
		ss << "</a></li>\r\n";
	}

	DIR *pDir;
	dirent *pDirent;
	if ((pDir = opendir(strPathPrefix.data())) == NULL) {
		return false;
	}
	set<string> setFile;
	while ((pDirent = readdir(pDir)) != NULL) {
		if (File::is_special_dir(pDirent->d_name)) {
			continue;
		}
		if(pDirent->d_name[0] == '.'){
			continue;
		}
		setFile.emplace(pDirent->d_name);
	}
	int i = 0;
	for(auto &strFile :setFile ){
		string strAbsolutePath = strPathPrefix + "/" + strFile;
		bool isDir = File::is_dir(strAbsolutePath.data());
		ss << "<li><span>" << i++ << "</span>\t";
		ss << "<a href=\"";
		if(!last_dir_name.empty()){
			ss << last_dir_name << "/" << strFile;
		}else{
			ss << strFile;
		}

		if(isDir){
			ss << "/";
		}
		ss << "\">";
		ss << strFile;
		if (isDir) {
			ss << "/</a></li>\r\n";
			continue;
		}
		//是文件
		struct stat fileData;
		if (0 == stat(strAbsolutePath.data(), &fileData)) {
			auto &fileSize = fileData.st_size;
			if (fileSize < 1024) {
				ss << " (" << fileData.st_size << "B)" << endl;
			} else if (fileSize < 1024 * 1024) {
				ss << fixed << setprecision(2) << " (" << fileData.st_size / 1024.0 << "KB)";
			} else if (fileSize < 1024 * 1024 * 1024) {
				ss << fixed << setprecision(2) << " (" << fileData.st_size / 1024 / 1024.0 << "MB)";
			} else {
				ss << fixed << setprecision(2) << " (" << fileData.st_size / 1024 / 1024 / 1024.0 << "GB)";
			}
		}
		ss << "</a></li>\r\n";
	}
	closedir(pDir);
	ss << "<ul>\r\n";
	ss << "</ul>\r\n</body></html>";
	ss.str().swap(strRet);
	return true;
}
inline void HttpSession::sendResponse(const char* pcStatus, const KeyValue& header, const string& strContent) {
	_StrPrinter printer;
	printer << "HTTP/1.1 " << pcStatus << "\r\n";
	for (auto &pr : header) {
		printer << pr.first << ": " << pr.second << "\r\n";
	}
	printer << "\r\n" << strContent;
	auto strSend = printer << endl;
	send(strSend);
	_ticker.resetTime();
}
inline HttpSession::KeyValue HttpSession::makeHttpHeader(bool bClose, int64_t iContentSize,const char* pcContentType) {
	KeyValue headerOut;
    GET_CONFIG(string,charSet,Http::kCharSet);
    GET_CONFIG(uint32_t,keepAliveSec,Http::kKeepAliveSecond);
    GET_CONFIG(uint32_t,reqCnt,Http::kMaxReqCount);

    //chenxiaolei  请求跨域支持
	headerOut.emplace("Access-Control-Allow-Headers", "DNT,X-CustomHeader,Keep-Alive,User-Agent,X-Requested-With,If-Modified-Since,Cache-Control,Content-Type,Authorization");
	headerOut.emplace("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS, DELETE");
	headerOut.emplace("Access-Control-Allow-Origin", "*");
	headerOut.emplace("Access-Control-Expose-Headers", "Location");

	headerOut.emplace("Date", dateStr());
	headerOut.emplace("Server", SERVER_NAME);
	headerOut.emplace("Connection", bClose ? "close" : "keep-alive");
	if(!bClose){
		headerOut.emplace("Keep-Alive",StrPrinter << "timeout=" << keepAliveSec << ", max=" << reqCnt << endl);
	}
	if(pcContentType){
		auto strContentType = StrPrinter << pcContentType << "; charset=" << charSet << endl;
		headerOut.emplace("Content-Type",strContentType);
	}
	if(iContentSize > 0){
		headerOut.emplace("Content-Length", StrPrinter<<iContentSize<<endl);
	}
	return headerOut;
}

string HttpSession::urlDecode(const string &str){
	auto ret = strCoding::UrlDecode(str);
#ifdef _WIN32
    GET_CONFIG(string,charSet,Http::kCharSet);
	bool isGb2312 = !strcasecmp(charSet.data(), "gb2312");
	if (isGb2312) {
		ret = strCoding::UTF8ToGB2312(ret);
	}
#endif // _WIN32
    return ret;
}

inline void HttpSession::urlDecode(Parser &parser){
	parser.setUrl(urlDecode(parser.Url()));
	for(auto &pr : _parser.getUrlArgs()){
		const_cast<string &>(pr.second) = urlDecode(pr.second);
	}
}

inline bool HttpSession::emitHttpEvent(bool doInvoke){
	///////////////////是否断开本链接///////////////////////
    GET_CONFIG(uint32_t,reqCnt,Http::kMaxReqCount);

    bool bClose = (strcasecmp(_parser["Connection"].data(),"close") == 0) || ( ++_iReqCnt > reqCnt);
	auto Origin = _parser["Origin"];
	/////////////////////异步回复Invoker///////////////////////////////
	weak_ptr<HttpSession> weakSelf = dynamic_pointer_cast<HttpSession>(shared_from_this());
	HttpResponseInvoker invoker = [weakSelf,bClose,Origin](const string &codeOut, const KeyValue &headerOut, const string &contentOut){
		auto strongSelf = weakSelf.lock();
		if(!strongSelf) {
			return;
		}
		strongSelf->async([weakSelf,bClose,codeOut,headerOut,contentOut,Origin]() {
			auto strongSelf = weakSelf.lock();
			if(!strongSelf) {
				return;
			}
			strongSelf->responseDelay(Origin,bClose,codeOut,headerOut,contentOut);
			if(bClose){
				strongSelf->shutdown(SockException(Err_shutdown,"Connection: close"));
			}
		});
	};
	///////////////////广播HTTP事件///////////////////////////
	bool consumed = false;//该事件是否被消费
	NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastHttpRequest,_parser,invoker,consumed,*this);
	if(!consumed && doInvoke){
		//该事件无人消费，所以返回404
		invoker("404 Not Found",KeyValue(),"");
		if(bClose){
			//close类型，回复完毕，关闭连接
			shutdown(SockException(Err_shutdown,"404 Not Found"));
		}
	}
	return consumed;
}

//chenxiaolei  增加OPTIONS,以便支持web 页面的跨域嗅探请求
inline void HttpSession::Handle_Req_OPTIONS(int64_t &content_len) {
    sendResponse( "200 OK"  , makeHttpHeader(false, 0), "");
    shutdown(SockException(Err_shutdown,"recv http content completed"));
}

inline void HttpSession::Handle_Req_POST(int64_t &content_len) {
	GET_CONFIG(uint64_t,maxReqSize,Http::kMaxReqSize);
    GET_CONFIG(int,maxReqCnt,Http::kMaxReqCount);

    int64_t totalContentLen = _parser["Content-Length"].empty() ? -1 : atoll(_parser["Content-Length"].data());

	if(totalContentLen == 0){
		//content为空
		//emitHttpEvent内部会选择是否关闭连接
		emitHttpEvent(true);
		return;
	}

    //根据Content-Length设置接收缓存大小
    if(totalContentLen > 0){
        _sock->setReadBuffer(std::make_shared<BufferRaw>(MIN(totalContentLen + 1,256 * 1024)));
    }else{
	    //不定长度的Content-Length
        _sock->setReadBuffer(std::make_shared<BufferRaw>(256 * 1024));
	}

    if(totalContentLen > 0 && totalContentLen < maxReqSize ){
		//返回固定长度的content
		content_len = totalContentLen;
		auto parserCopy = _parser;
		_contentCallBack = [this,parserCopy](const char *data,uint64_t len){
			//恢复http头
			_parser = parserCopy;
			//设置content
			_parser.setContent(string(data,len));
			//触发http事件，emitHttpEvent内部会选择是否关闭连接
			emitHttpEvent(true);
			//清空数据,节省内存
			_parser.Clear();
			//content已经接收完毕
			return false;
		};
	}else{
		//返回不固定长度的content
		content_len = -1;
		auto parserCopy = _parser;
		std::shared_ptr<uint64_t> recvedContentLen = std::make_shared<uint64_t>(0);
		bool bClose = (strcasecmp(_parser["Connection"].data(),"close") == 0) || ( ++_iReqCnt > maxReqCnt);

		_contentCallBack = [this,parserCopy,totalContentLen,recvedContentLen,bClose](const char *data,uint64_t len){
		    *(recvedContentLen) += len;

		    onRecvUnlimitedContent(parserCopy,data,len,totalContentLen,*(recvedContentLen));

			if(*(recvedContentLen) < totalContentLen){
			    //数据还没接收完毕
                //_contentCallBack是可持续的，后面还要处理后续content数据
                return true;
			}

			//数据接收完毕
            if(!bClose){
			    //keep-alive类型连接
				//content接收完毕，后续都是http header
				setContentLen(0);
                //content已经接收完毕
                return false;
            }

            //连接类型是close类型，收完content就关闭连接
            shutdown(SockException(Err_shutdown,"recv http content completed"));
            //content已经接收完毕
            return false ;
		};
	}
	//有后续content数据要处理,暂时不关闭连接
}
void HttpSession::responseDelay(const string &Origin,bool bClose,
								const string &codeOut,const KeyValue &headerOut,
								const string &contentOut){
	if(codeOut.empty()){
		sendNotFound(bClose);
		return;
	}
	auto headerOther=makeHttpHeader(bClose,contentOut.size(),"text/plain");
	if(!Origin.empty()){
		headerOther["Access-Control-Allow-Origin"] = Origin;
		headerOther["Access-Control-Allow-Credentials"] = "true";
	}

    for (auto &pr : headerOther){
        //添加默认http头，默认http头不能覆盖用户自定义的头
        const_cast<KeyValue &>(headerOut).emplace(pr.first,pr.second);
    }
	sendResponse(codeOut.data(), headerOut, contentOut);
}
inline void HttpSession::sendNotFound(bool bClose) {
    GET_CONFIG(string,notFound,Http::kNotFound);
    sendResponse("404 Not Found", makeHttpHeader(bClose, notFound.size()), notFound);
}


void HttpSession::onWrite(const Buffer::Ptr &buffer) {
	_ticker.resetTime();
	_ui64TotalBytes += buffer->size();
	send(buffer);
}

void HttpSession::onDetach() {
	shutdown(SockException(Err_shutdown,"rtmp ring buffer detached"));
}

std::shared_ptr<FlvMuxer> HttpSession::getSharedPtr(){
	return dynamic_pointer_cast<FlvMuxer>(shared_from_this());
}

} /* namespace mediakit */
