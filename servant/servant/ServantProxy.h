﻿/**
 * Tencent is pleased to support the open source community by making Tars available.
 *
 * Copyright (C) 2016THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use this file except 
 * in compliance with the License. You may obtain a copy of the License at
 *
 * https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software distributed 
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR 
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the 
 * specific language governing permissions and limitations under the License.
 */

#ifndef _TARS_SERVANT_PROXY_H_
#define _TARS_SERVANT_PROXY_H_

#include "util/tc_monitor.h"
#include "util/tc_autoptr.h"
#include "servant/Message.h"
#include "servant/AppProtocol.h"
#include "servant/TarsCurrent.h"
//#include "servant/EndpointInfo.h"
#include "servant/CommunicatorEpoll.h"

namespace tars
{

class EndpointInfo;

/////////////////////////////////////////////////////////////////////////
/*
 * seq 管理的类
 */
class SeqManager
{
public:
    const static uint16_t MAX_UNSIGN_SHORT = 0xffff;

    struct SeqInfo
    {
        uint16_t next;
        bool     free;
    };

    /**
     * 构造函数
     */
    SeqManager(uint16_t iNum);

    /**
     * 获取seq
     */
    uint16_t get();

    /**
     * 删除seq
     */
    void del(uint16_t iSeq);

private:
    uint16_t    _num;

    uint16_t    _free;

    uint16_t    _freeTail;

    SeqInfo  *  _p;

    TC_SpinLock _mutex;
};

/////////////////////////////////////////////////////////////////////////
/*
 * 线程私有数据
 */
class ServantProxyThreadData
{
public:
    static TC_SpinLock _mutex;
    static SeqManager *_pSeq;

    static thread_local shared_ptr<ServantProxyThreadData> g_sp;

    /**
     * 构造函数
     */
    ServantProxyThreadData();

    /**
     * 析构函数
     */
    ~ServantProxyThreadData();

    /**
     * 获取线程数据，没有的话会自动创建
     * @return ServantProxyThreadData*
     */
    static ServantProxyThreadData * getData();

public:
    /*
     * 每个线程跟客户端网络线程通信的队列
     */
    ReqInfoQueue * _reqQueue[MAX_CLIENT_THREAD_NUM]; //队列数组
    bool           _queueInit;                       //是否初始化
    uint16_t       _reqQNo;                          //请求事件通知的seq
    size_t         _netSeq;                          //轮训选择网络线程的偏移量
    int            _netThreadSeq;                     //网络线程发起的请求回到自己的网络线程来处理,其值为网络线程的id

    /**
     * hash属性,客户端每次调用都进行设置
     */
    bool           _hash;                            //是否普通取模hash
    bool           _conHash;                          //是否一致性hash
    int64_t        _hashCode;                        //hash值

    /**
     * 染色信息
     */
    bool           _dyeing;                          //标识当前线程是否需要染色
    string         _dyeingKey;                        //染色的key值

    /**
     * 允许客户端设置接口级别的超时时间,包括同步和异步、单向
     */
    bool           _hasTimeout;                      //是否设置超时间
    int            _timeout;                         //超时时间

    /**
     * 保存调用后端服务的地址信息
     */
    string           _szHost;                       //调用对端地址

    /**
     * 协程调度
     */
    CoroutineScheduler*        _sched;                   //协程调度器

    /**
     * ObjectProxy
     */
    size_t         _objectProxyNum;                  //ObjectProxy对象的个数，其个数由客户端的网络线程数决定，每个网络线程有一个ObjectProxy

    /**
     *  objectProxy Pointer
     */
    shared_ptr<ObjectProxy *> _objectProxyOwn;                    //保存ObjectProxy对象的指针数组
#ifdef _USE_OPENTRACKING
    std::unordered_map<std::string, std::string> _trackInfoMap;
#endif
};


//////////////////////////////////////////////////////////////////////////
// 协程并行请求的基类
class CoroParallelBase : virtual public TC_HandleBase
{
public:
    /**
     * 构造
     */
    CoroParallelBase(int iNum)
    : _num(iNum)
    , _count(iNum)
    , _req_count(0)
    {}

    /**
     * 析构函数
     */
    virtual ~CoroParallelBase() {}

    /**
     * 增加调用协程接口请求的数目
     */
    int incReqCount() { return (++_req_count); }

    /**
     * 判断协程并行请求数目是否都发送了
     */
    bool checkAllReqSend() { return _num == _req_count; }

    /**
     * 判断协程并行请求是否都回来了
     */
    bool checkAllReqReturn() { return (--_count) == 0; }

    /**
     * 获取所有请求回来的响应
     */
    vector<ReqMessage*> getAllReqMessage()
    {
        vector<ReqMessage*> vRet;

        {
            TC_LockT<TC_SpinLock> lock(_mutex);
            vRet.swap(_vReqMessage);
			_vReqMessage.clear();
        }

        return vRet;
    }

    /**
     * 插入请求回来的响应
     */
    void insert(ReqMessage* msg)
    {
        TC_LockT<TC_SpinLock> lock(_mutex);
        _vReqMessage.push_back(msg);
    }

protected:
    /**
     * 并行请求的数目
     */
    int                        _num;

    /**
     * 并行请求的响应还未回来的数目
     */
    std::atomic<int>     _count;

    /**
     * 并行请求的已发送的数目
     */
    std::atomic<int>      _req_count;

    /**
     * 互斥锁
     */
    TC_SpinLock            _mutex;

    /**
     * 请求的响应的容器
     */
    vector<ReqMessage*>        _vReqMessage;
};
typedef TC_AutoPtr<CoroParallelBase> CoroParallelBasePtr;

//等待所有的请求回来
void coroWhenAll(const CoroParallelBasePtr &ptr);

//////////////////////////////////////////////////////////////////////////
// 异步回调对象的基类
class ServantProxyCallback : virtual public TC_HandleBase
{
public:
    /**
     * 构造
     */
    ServantProxyCallback();

    /**
     * 析构函数
     */
    virtual ~ServantProxyCallback() {}

    /**
     * 获取类型
     * @return const string&
     */
    virtual const string& getType() { return _type; }

    /**
     * 设置类型
     * @return const string&
     */
    virtual void setType(const string& type) { _type = type; }

    /**
     * 设置coro并行请求的共享智能指针
     */
    virtual void setCoroParallelBasePtr(tars::CoroParallelBasePtr pPtr) { _pPtr = pPtr; }

    /**
     * 获取coro并行请求的共享智能指针
     */
    virtual const CoroParallelBasePtr& getCoroParallelBasePtr() { return _pPtr; }

    /**
     * 异步请求是否在网络线程处理
     * tars内部用的到 业务不能设置这个值
     * */
    inline void setNetThreadProcess(bool bNetThreadProcess)
    {
        _bNetThreadProcess = bNetThreadProcess;
    }

    inline bool getNetThreadProcess()
    {
        return _bNetThreadProcess;
    }

public:
    /**
     * 异步回调对象实现该方法，进行业务逻辑处理
     * @param msg
     * @return int
     */
    virtual int onDispatch(ReqMessagePtr ptr) = 0;

    /**
     * 连接关闭掉了(只对PUSH callback生效)
     */    
    virtual void onClose(){};

	/**
	 * 连接已建立(只对PUSH callback生效)
	 */
	virtual void onConnect(const TC_Endpoint &ep){};

protected:

    /**
     * 同一链路多个cb的时候可以用来区分class类型
     */
    string _type;

    /**
     * 异步请求是否在网络线程处理
     * tars内部用的到 业务不能设置这个值
     * */
    bool _bNetThreadProcess;

    /**
     * 协程并行请求的共享智能指针
     */
    tars::CoroParallelBasePtr _pPtr;
};
///////////////////////////////////////////////////////////////////////////////////////////////
// for http
class HttpCallback : public TC_HandleBase
{
public:
    virtual int onHttpResponse(const std::map<std::string, std::string>& requestHeaders ,
                               const std::map<std::string, std::string>& responseHeaders ,
                               const std::vector<char>& rspBody) = 0;
    virtual int onHttpResponseException(const std::map<std::string, std::string>& requestHeaders,
                                        int expCode) = 0;
};

typedef TC_AutoPtr<HttpCallback> HttpCallbackPtr;

class HttpServantProxyCallback : virtual public ServantProxyCallback
{
public:
    explicit HttpServantProxyCallback(const HttpCallbackPtr& cb);

   /**
     * 异步回调对象实现该方法，进行业务逻辑处理
     * @param msg
     * @return int
     */
    virtual int onDispatch(ReqMessagePtr ptr);

    /**
     * 异步回调对象实现该方法，进行业务逻辑处理
     * @param msg
     * @return void
     */
    virtual int onDispatchResponse(const RequestPacket &req, const ResponsePacket &rsp);

    /**
     * 异步回调对象实现该方法(异常)，进行业务逻辑处理
     * @param msg
     * @return void
     */
    virtual int onDispatchException(const RequestPacket &req, const ResponsePacket &rsp);


private:
    HttpCallbackPtr _httpCb;
};

//////////////////////////////////////////////////////////////////////////
/**
 * 1:远程对象的本地代理
 * 2:同名servant在一个通信器中最多只有一个实例
 * 3:防止和用户在Tars中定义的函数名冲突，接口以tars_开头
 */
class EndpointManagerThread;

class ServantProxy : public TC_HandleBase, public TC_ThreadMutex
{
public:
    /**
     * 通过status传递数据时用到的缺省字符串
     */
    static string STATUS_DYED_KEY;  //需要染色的用户ID

    static string STATUS_GRID_KEY;  //需要灰度的用户ID

    static string STATUS_SAMPLE_KEY; //stat 采样的信息

    static string STATUS_RESULT_CODE; //处理结果码,tup使用

    static string STATUS_RESULT_DESC; //处理结果描述,tup使用

    static string STATUS_SETNAME_VALUE; //set调用

    static string TARS_MASTER_KEY; //透传主调名称信息

    static string STATUS_TRACK_KEY; //track信息

    /**
     * 缺省的同步调用超时时间
     * 超时后不保证消息不会被服务端处理
     */
    enum { DEFAULT_SYNCTIMEOUT = 3000, DEFAULT_ASYNCTIMEOUT=5000};

    /**
     * 构造函数
     * @param op
     */
    ServantProxy(Communicator * pCommunicator, ObjectProxy ** ppObjectProxy, size_t iClientThreadNum);

    /**
     * 析构函数
     */
    virtual ~ServantProxy();
public:

    /**
     * 获取Object可用服务列表 如果启用set则获取本set的,如果启用分组,只返回同分组的服务端ip
     *  @return void
     */
    void tars_endpoints(vector<EndpointInfo> &activeEndPoint, vector<EndpointInfo> &inactiveEndPoint);

    /**
     * 获取Object可用服务列表 所有的列表
     *  @return void
     */
    void tars_endpointsAll(vector<EndpointInfo> &activeEndPoint, vector<EndpointInfo> &inactiveEndPoint);

    /**
     * 获取Object可用服务列表 根据set名字获取
     *  @return void
     */
    void tars_endpointsBySet(const string & sName,vector<EndpointInfo> &activeEndPoint, vector<EndpointInfo> &inactiveEndPoint);

    /**
     * 获取Object可用服务列表 根据地区名字获取
     *  @return void
     */
    void tars_endpointsByStation(const string & sName,vector<EndpointInfo> &activeEndPoint, vector<EndpointInfo> &inactiveEndPoint);

    /**
     *  获取Object可用服务列表 包括指定归属地
     *  @return vector<TC_Endpoint>
     **/
    vector<TC_Endpoint> tars_endpoints(const std::string & sStation);

    /**
     * 获取Object可用服务列表 包括指定归属地
     *  @return void
     */
    void tars_endpoints(const std::string & sStation, vector<TC_Endpoint> & vecActive, vector<TC_Endpoint> & vecInactive);

    /**
     * 获取Object可用服务列表 如果启用分组,只返回同分组的服务端ip
     * @return vector<TC_Endpoint>
     */
    vector<TC_Endpoint> getEndpoint();

    /**
     * 获取Object可用服务列表 包括所有IDC
     * @return vector<TC_Endpoint>
     */
    vector<TC_Endpoint> getEndpoint4All();

public:

    /**
     * 获取通信器
     *
     * @return Communicator*
     */
    Communicator* tars_communicator() { return _communicator; }

    /**
     * 发送测试消息到服务器
     */
    void tars_ping();

    /**
     * 异步ping, 不等回包
     */
	void tars_async_ping();

    /**
     * 设置同步调用超时时间，对该proxy上所有方法都有效
     * @param msecond
     */
    void tars_timeout(int msecond);

    /**
     * 获取同步调用超时时间，对该proxy上所有方法都有效
     * @return int
     */
    int tars_timeout() const;

    /**
     * 设置连接超时时间
     * @return int
     */
    void tars_connect_timeout(int conTimeout);

	/**
	 * set auto reconnect time
	 * @return int, second
	 */
    void tars_reconnect(int second);

    /**
     * 获取所属的Object名称
     * @return string
     */
    string tars_name() const;

    /**
     * 获取最近一次调用的IP地址和端口
     * @return string
     */
    static TC_Endpoint tars_invoke_endpoint();

    /**
     * 设置用户自定义协议
     * @param protocol
     */
    void tars_set_protocol(const ProxyProtocol& protocol);

    /**
    *设置套接字选项
    */
    void tars_set_sockopt(int level, int optname, const void *optval, SOCKET_LEN_TYPE optlen);

    /**
     * 设置超时检查参数
     */
    void tars_set_check_timeout(const CheckTimeoutInfo& checkTimeoutInfo);

    /**
     * 获取超时检查参数
     */
    CheckTimeoutInfo tars_get_check_timeout();

    /**
     * hash方法，为保证一段时间内同一个key的消息发送
     * 到相同的服务端，由于服务列表动态变化，所以
     * 不严格保证
     * @param key
     * @return ServantProxy*
     */
    virtual ServantProxy* tars_hash(int64_t key);

    /**
     * 一致性hash方法
     */
    virtual ServantProxy* tars_consistent_hash(int64_t key);

//    /**
//     * 直接同步调用
//     * @return
//     */
//	virtual ServantProxy* taf_direct();

	/**
     * 清除当前的Hash数据
     * 空函数 为了兼容以前的
     * @param key
     * @return ServantProxy*
     */
    void tars_clear_hash();

    /**
     * 针对客户端调用接口级别的超时时间设置，包括同步和异步调用
     * 每次接口调用都必须设置，否则取系统默认超时时间
     *
     * @param msecond 单位毫秒
     * @return ServantProxy*
     * 示例: prxy->tars_set_timeout(3000)->sayHello();
     */
    virtual ServantProxy* tars_set_timeout(int msecond);

    /**
     * 设置异步调用超时时间，对该proxy上所有方法都有效
     * @param msecond
     */
    void tars_async_timeout(int msecond);

    /**
     * 获取异步调用超时时间，对该proxy上所有方法都有效
     * @return int
     */
    int tars_async_timeout() const;

    /**
     * 用proxy产生一个该object上的序列号
     * @return uint32_t
     */
    virtual uint32_t tars_gen_requestid();

    /**
     * 设置PUSH类消息的响应callback
     * @param cb
     */
    virtual void tars_set_push_callback(const ServantProxyCallbackPtr& cb);


    /**
     * 普通协议同步远程调用
     */
    virtual void rpc_call(uint32_t requestId, const string& sFuncName,
                          const char* buff, uint32_t len, ResponsePacket &rsp);

    /**
     * 普通协议异步调用
     */
    virtual void rpc_call_async(uint32_t requestId, const string& sFuncName,
                                const char* buff, uint32_t len,
                                const ServantProxyCallbackPtr& callback,
                                bool bCoro = false);
//
//
//	/**
//	 * http1同步远程调用
//	 */
//	void http1_call(const std::string& method,
//	               const std::string& uri,
//	               const std::map<std::string, std::string>& headers,
//	               const std::string& body,
//	               std::map<std::string, std::string>& rheaders,
//	               std::string& rbody);

    /**
     * http2协议同步远程调用
     */
    void http_call(const std::string& method,
                    const std::string& uri,
                    const std::map<std::string, std::string>& headers,
                    const std::string& body,
                    std::map<std::string, std::string>& rheaders,
                    std::string& rbody);
    /**
     * http2协议异步远程调用
     */
    void http_call_async(const std::string& method,
                         const std::string& uri,
                         const std::map<std::string, std::string>& headers,
                          const std::string& body,
                          const HttpCallbackPtr &cb);

    /**
     * 在RequestPacket中的context设置主调信息标识
     */
    virtual void tars_setMasterFlag(bool bMasterFlag) {_masterFlag = bMasterFlag;}

    /**
     * TARS协议同步方法调用
     */
    virtual shared_ptr<ResponsePacket> tars_invoke(char cPacketType,
                            const string& sFuncName,
                            tars::TarsOutputStream<tars::BufferWriterVector>& buf,
                            const map<string, string>& context,
                            const map<string, string>& status);

    /**
     * TARS协议异步方法调用
     */
    virtual void tars_invoke_async(char cPacketType,
                                  const string& sFuncName,
                                  tars::TarsOutputStream<tars::BufferWriterVector> &buf,
                                  const map<string, string>& context,
                                  const map<string, string>& status,
                                  const ServantProxyCallbackPtr& callback,
                                  bool bCoro = false);

private:
    /**
     * 远程方法调用
     * @param req
     * @return int
     */
    void invoke(ReqMessage *msg, bool bCoroAsync = false);

//    /**
//     * invoke 异步
//     * @param msg
//     * @param pSptd
//     * @param pReqQ
//     * @param bCoroAsync
//     */
//    void invoke_async(ReqMessage *msg, ServantProxyThreadData *pSptd,  ReqInfoQueue *pReqQ, bool bCoroAsync);

//    /**
//     * invoke 同步
//     * @param msg
//     * @param pSptd
//     * @param pReqQ
//     */
//	void invoke_sync(ReqMessage *msg, ServantProxyThreadData *pSptd,  ReqInfoQueue *pReqQ);

	/**
     * 选取一个网络线程对应的信息
     * @param pSptd
     * @return void
     */
    void selectNetThreadInfo(ServantProxyThreadData * pSptd, ObjectProxy * & pObjProxy, ReqInfoQueue * & pReqQ);

    /**
     * 检查是否需要设置染色消息
     * @param  req
     */
    void checkDye(RequestPacket& req);
private:
    friend class ObjectProxy;
    friend class AdapterProxy;

    /**
     * 通信器
     */
    Communicator *            _communicator;

    /**
     * 保存ObjectProxy对象的指针数组
     */
    ObjectProxy ** _objectProxy;                    //保存ObjectProxy对象的指针数组
    shared_ptr<ObjectProxy *> _objectProxyOwn;                    //保存ObjectProxy对象的指针数组

    /**
     * ObjectProxy对象的个数，其个数由客户端的网络线程数决定，
     * 每个网络线程有一个ObjectProxy
     */
    size_t                    _objectProxyNum;

    /**
     * 同步调用超时(毫秒)
     */
    int                        _syncTimeout;

    /**
     * 同步调用超时(毫秒)
     */
    int                        _asyncTimeout;

    /**
     * 唯一id
     */
    std::atomic<uint32_t>      _id;

    /**
     * 获取endpoint对象
     */
    std::unique_ptr<EndpointManagerThread> _endpointInfo;
    
    /**
     * 是否在RequestPacket中的context设置主调信息
     */
    bool                    _masterFlag;

    /*
     *最小的超时时间
     */
    int64_t                    _minTimeout;
};
}
#endif
