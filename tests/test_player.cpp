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
#include <signal.h>
#include <unistd.h>
#include "Util/util.h"
#include "Util/logger.h"
#include <iostream>
#include "Poller/EventPoller.h"
#include "Rtsp/UDPServer.h"
#include "Player/MediaPlayer.h"
#include "Util/onceToken.h"
#include "H264Decoder.h"
#include "YuvDisplayer.h"
#include "Network/sockutil.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

int main(int argc, char *argv[]) {
    //设置退出信号处理函数
    signal(SIGINT, [](int) { SDLDisplayerHelper::Instance().shutdown(); });
    //设置日志
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    if (argc != 3) {
        ErrorL << "\r\n测试方法：./test_player rtxp_url rtp_type\r\n"
               << "例如：./test_player rtsp://admin:123456@127.0.0.1/live/0 0\r\n"
               << endl;
        return 0;

    }

    MediaPlayer::Ptr player(new MediaPlayer());
    weak_ptr<MediaPlayer> weakPlayer = player;
    player->setOnPlayResult([weakPlayer](const SockException &ex) {
        InfoL << "OnPlayResult:" << ex.what();
        auto strongPlayer = weakPlayer.lock();
        if (ex || !strongPlayer) {
            return;
        }

        auto viedoTrack = strongPlayer->getTrack(TrackVideo);
        if (!viedoTrack || viedoTrack->getCodecId() != CodecH264) {
            WarnL << "没有视频或者视频不是264编码!";
            return;
        }

        AnyStorage::Ptr storage(new AnyStorage);
        viedoTrack->addDelegate(std::make_shared<FrameWriterInterfaceHelper>([storage](const Frame::Ptr &frame) {
            SDLDisplayerHelper::Instance().doTask([frame,storage]() {
                auto &decoder = (*storage)["decoder"];
                auto &displayer = (*storage)["displayer"];
                if(!decoder){
                    decoder.set<H264Decoder>();
                }
                if(!displayer){
                    displayer.set<YuvDisplayer>();
                }

                AVFrame *pFrame = nullptr;
                bool flag = decoder.get<H264Decoder>().inputVideo((unsigned char *) frame->data(), frame->size(), frame->stamp(), &pFrame);
                if (flag) {
                    displayer.get<YuvDisplayer>().displayYUV(pFrame);
                }
                return true;
            });
        }));
    });


    player->setOnShutdown([](const SockException &ex) {
        ErrorL << "OnShutdown:" << ex.what();
    });
    (*player)[kRtpType] = atoi(argv[2]);
    player->play(argv[1]);

    SDLDisplayerHelper::Instance().runLoop();
    return 0;
}

