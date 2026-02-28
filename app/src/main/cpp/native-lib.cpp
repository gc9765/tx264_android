#include <jni.h>
#include <string>
#include <android/log.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavutil/error.h"
}

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TX_VideoT", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "TX_VideoT", __VA_ARGS__)

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_myvideoapp_MainActivity_stringFromJNI(JNIEnv* env, jobject) {
    return env->NewStringUTF(av_version_info());
}

// 视频转码（含音频复制）- HEVC解码修复版
extern "C" JNIEXPORT jint JNICALL
Java_com_example_myvideoapp_MainActivity_transcodeVideo(
        JNIEnv* env, jobject, jstring inputPath, jstring outputPath) {
    
    const char* input = env->GetStringUTFChars(inputPath, nullptr);
    const char* output = env->GetStringUTFChars(outputPath, nullptr);
    
    LOGI("开始转码: %s -> %s", input, output);
    
    // 所有变量在函数开头声明
    AVFormatContext *inFmtCtx = nullptr, *outFmtCtx = nullptr;
    AVCodecContext *decCtx = nullptr, *encCtx = nullptr;
    SwsContext *swsCtx = nullptr;
    AVStream *outStream = nullptr;
    AVStream *outAudioStream = nullptr;
    const AVCodec *decCodec = nullptr;
    const AVCodec *encCodec = nullptr;
    AVFrame *decFrame = nullptr;
    AVFrame *encFrame = nullptr;
    AVPacket *pkt = nullptr;
    AVPacket *encPkt = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    int ret = 0;
    int64_t frameCount = 0;
    int audioPacketCount = 0;
    int videoPacketCount = 0;
    int flushCount = 0;
    char errbuf[256];
    
    // 1. 打开输入文件
    if ((ret = avformat_open_input(&inFmtCtx, input, nullptr, nullptr)) < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法打开输入文件: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    
    if ((ret = avformat_find_stream_info(inFmtCtx, nullptr)) < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法获取流信息: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    
    // 2. 查找视频流和音频流
    for (unsigned int i = 0; i < inFmtCtx->nb_streams; i++) {
        if (inFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            LOGI("找到视频流 #%d: %s", i, avcodec_get_name(inFmtCtx->streams[i]->codecpar->codec_id));
        } else if (inFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStream = i;
            LOGI("找到音频流 #%d: %s, %dHz, %d channels", 
                 i, 
                 avcodec_get_name(inFmtCtx->streams[i]->codecpar->codec_id),
                 inFmtCtx->streams[i]->codecpar->sample_rate,
                 inFmtCtx->streams[i]->codecpar->ch_layout.nb_channels);
        }
    }
    
    if (videoStream == -1) {
        LOGE("找不到视频流");
        ret = -1;
        goto cleanup;
    }
    
    if (audioStream == -1) {
        LOGI("警告: 未找到音频流，将只转码视频");
    }
    
    // 3. 初始化解码器（支持HEVC/H.265）
    decCodec = avcodec_find_decoder(inFmtCtx->streams[videoStream]->codecpar->codec_id);
    if (!decCodec) {
        LOGE("找不到解码器: %s", avcodec_get_name(inFmtCtx->streams[videoStream]->codecpar->codec_id));
        ret = -1;
        goto cleanup;
    }
    
    decCtx = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(decCtx, inFmtCtx->streams[videoStream]->codecpar);
    
    // 对于HEVC，启用低延迟解码（避免缓冲导致EAGAIN）
    decCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    
    if ((ret = avcodec_open2(decCtx, decCodec, nullptr)) < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法打开解码器: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    LOGI("解码器已打开: %s", decCodec->name);
    
    // 4. 创建输出上下文
    avformat_alloc_output_context2(&outFmtCtx, nullptr, nullptr, output);
    if (!outFmtCtx) {
        LOGE("无法创建输出上下文");
        ret = -1;
        goto cleanup;
    }
    
    // 5. 初始化 x264 编码器
    encCodec = avcodec_find_encoder_by_name("libx264");
    if (!encCodec) {
        LOGE("找不到 libx264 编码器");
        ret = -1;
        goto cleanup;
    }
    
    encCtx = avcodec_alloc_context3(encCodec);
    
    // 基础参数（720x480）
    encCtx->width = 720;
    encCtx->height = 480;
    encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    encCtx->time_base = (AVRational){1, 30};
    encCtx->framerate = (AVRational){30, 1};
    encCtx->bit_rate = 2000000;  // 改为2M避免文件过大
    
    // x264 参数
    av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(encCtx->priv_data, "profile", "baseline", 0);
    encCtx->gop_size = 30;
    encCtx->max_b_frames = 0;
    
    if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    LOGI("正在打开编码器: %dx%d @ %lld bps", encCtx->width, encCtx->height, encCtx->bit_rate);
    if ((ret = avcodec_open2(encCtx, encCodec, nullptr)) < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法打开编码器: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    LOGI("编码器打开成功");
    
    // 6. 添加视频流
    outStream = avformat_new_stream(outFmtCtx, nullptr);
    if (!outStream) {
        LOGE("无法创建视频输出流");
        ret = -1;
        goto cleanup;
    }
    
    ret = avcodec_parameters_from_context(outStream->codecpar, encCtx);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法复制视频编码器参数: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    outStream->time_base = encCtx->time_base;
    
    // 添加音频流
    if (audioStream != -1) {
        AVStream *inAudioStream = inFmtCtx->streams[audioStream];
        outAudioStream = avformat_new_stream(outFmtCtx, nullptr);
        if (!outAudioStream) {
            LOGE("无法创建音频输出流");
            ret = -1;
            goto cleanup;
        }
        
        ret = avcodec_parameters_copy(outAudioStream->codecpar, inAudioStream->codecpar);
        if (ret < 0) {
            LOGE("无法复制音频参数: %d", ret);
            goto cleanup;
        }
        outAudioStream->codecpar->codec_tag = 0;
        outAudioStream->time_base = inAudioStream->time_base;
        
        LOGI("音频流已添加: %s", avcodec_get_name(inAudioStream->codecpar->codec_id));
    }
    
    // 7. 打开输出文件
    if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&outFmtCtx->pb, output, AVIO_FLAG_WRITE)) < 0) {
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOGE("无法打开输出文件: %d (%s)", ret, errbuf);
            goto cleanup;
        }
    }
    
    if ((ret = avformat_write_header(outFmtCtx, nullptr)) < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法写入文件头: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    
    LOGI("文件头已写入，开始处理数据包");
    
    // 8. 初始化像素转换
    swsCtx = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt,
                            720, 480, encCtx->pix_fmt,
                            SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        LOGE("无法创建 SwsContext");
        ret = -1;
        goto cleanup;
    }
    
    // 9. 分配帧内存
    decFrame = av_frame_alloc();
    encFrame = av_frame_alloc();
    if (!decFrame || !encFrame) {
        LOGE("无法分配帧内存");
        ret = -1;
        goto cleanup;
    }
    
    encFrame->format = encCtx->pix_fmt;
    encFrame->width = 720;
    encFrame->height = 480;
    ret = av_frame_get_buffer(encFrame, 0);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("无法分配帧缓冲区: %d (%s)", ret, errbuf);
        goto cleanup;
    }
    
    pkt = av_packet_alloc();
    encPkt = nullptr;
    
    // 10. 读取、处理、写入循环（修复EAGAIN处理）
    while (av_read_frame(inFmtCtx, pkt) >= 0) {
        if (pkt->stream_index == videoStream) {
            videoPacketCount++;
            
            // 发送数据包到解码器（修复EAGAIN处理）
            ret = avcodec_send_packet(decCtx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                // 真正的错误（除了EAGAIN）
                LOGE("发送解码包错误: %d", ret);
                av_packet_unref(pkt);
                goto cleanup;
            }
            
            // 接收所有可用的解码帧
            while (ret >= 0) {
                ret = avcodec_receive_frame(decCtx, decFrame);
                
                if (ret == AVERROR(EAGAIN)) {
                    // 需要更多输入数据，这是正常的，跳出内层循环继续读包
                    break;
                }
                if (ret == AVERROR_EOF) {
                    LOGI("视频解码器到达EOF");
                    break;
                }
                if (ret < 0) {
                    LOGE("接收解码帧错误: %d", ret);
                    goto cleanup;
                }
                
                // 成功接收到帧，进行处理
                sws_scale(swsCtx, decFrame->data, decFrame->linesize, 0, 
                         decCtx->height, encFrame->data, encFrame->linesize);
                
                encFrame->pts = frameCount++;
                
                // 编码
                ret = avcodec_send_frame(encCtx, encFrame);
                if (ret < 0) {
                    LOGE("编码错误: %d", ret);
                    goto cleanup;
                }
                
                // 接收编码后的包
                encPkt = av_packet_alloc();
                while (ret >= 0) {
                    ret = avcodec_receive_packet(encCtx, encPkt);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) {
                        LOGE("接收编码包错误: %d", ret);
                        av_packet_free(&encPkt);
                        encPkt = nullptr;
                        goto cleanup;
                    }
                    
                    av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
                    encPkt->stream_index = outStream->index;
                    
                    ret = av_interleaved_write_frame(outFmtCtx, encPkt);
                    if (ret < 0) {
                        LOGE("写入视频帧错误: %d", ret);
                        av_packet_free(&encPkt);
                        encPkt = nullptr;
                        goto cleanup;
                    }
                }
                av_packet_free(&encPkt);
                encPkt = nullptr;
            }
            
            // 释放输入包
            av_packet_unref(pkt);
            
        } else if (pkt->stream_index == audioStream && outAudioStream) {
            audioPacketCount++;
            
            // 音频直接复制
            AVStream *inAudioStream = inFmtCtx->streams[audioStream];
            av_packet_rescale_ts(pkt, 
                inAudioStream->time_base,
                outAudioStream->time_base);
            pkt->stream_index = outAudioStream->index;
            pkt->pos = -1;
            
            ret = av_interleaved_write_frame(outFmtCtx, pkt);
            if (ret < 0) {
                LOGE("写入音频帧错误: %d", ret);
                av_packet_unref(pkt);
                break;
            }
        } else {
            // 其他流直接丢弃
            av_packet_unref(pkt);
        }
    }
    
    LOGI("读取完成: 视频包 %d 个, 音频包 %d 个", videoPacketCount, audioPacketCount);
    
    // 11. 刷新视频解码器（处理缓冲的帧）
    LOGI("刷新解码器...");
    avcodec_send_packet(decCtx, nullptr);  // 发送EOF
    while (true) {
        ret = avcodec_receive_frame(decCtx, decFrame);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) break;
        
        // 处理剩余帧
        sws_scale(swsCtx, decFrame->data, decFrame->linesize, 0, 
                 decCtx->height, encFrame->data, encFrame->linesize);
        encFrame->pts = frameCount++;
        
        avcodec_send_frame(encCtx, encFrame);
        encPkt = av_packet_alloc();
        while (avcodec_receive_packet(encCtx, encPkt) == 0) {
            av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
            encPkt->stream_index = outStream->index;
            av_interleaved_write_frame(outFmtCtx, encPkt);
        }
        av_packet_free(&encPkt);
    }
    
    // 12. 刷新视频编码器
    avcodec_send_frame(encCtx, nullptr);
    encPkt = av_packet_alloc();
    while (true) {
        ret = avcodec_receive_packet(encCtx, encPkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) break;
        
        av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
        encPkt->stream_index = outStream->index;
        av_interleaved_write_frame(outFmtCtx, encPkt);
        flushCount++;
    }
    av_packet_free(&encPkt);
    LOGI("刷新编码器: %d 帧", flushCount);
    
    // 13. 写入文件尾
    ret = av_write_trailer(outFmtCtx);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGE("写入文件尾失败: %d (%s)", ret, errbuf);
    } else {
        LOGI("转码完成: 视频 %ld 帧, 音频 %d 包", frameCount, audioPacketCount);
        ret = 0;
    }
    
cleanup:
    if (encPkt) av_packet_free(&encPkt);
    if (pkt) av_packet_free(&pkt);
    if (decFrame) av_frame_free(&decFrame);
    if (encFrame) av_frame_free(&encFrame);
    if (swsCtx) sws_freeContext(swsCtx);
    if (encCtx) avcodec_free_context(&encCtx);
    if (decCtx) avcodec_free_context(&decCtx);
    if (outFmtCtx && !(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&outFmtCtx->pb);
    }
    if (outFmtCtx) avformat_free_context(outFmtCtx);
    if (inFmtCtx) avformat_close_input(&inFmtCtx);
    
    env->ReleaseStringUTFChars(inputPath, input);
    env->ReleaseStringUTFChars(outputPath, output);
    
    return ret >= 0 ? 0 : -1;
}
