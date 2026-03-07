package com.youkong.audio_server.dto;

import lombok.Data;

import java.util.List;
import java.util.Map;

/**
 * WebRTC 信令消息 DTO
 */
@Data
public class SignalingMessage {

    /**
     * 信令类型
     */
    private String type;

    /**
     * 目标设备/会话ID
     */
    private String target="web_client_001";

    /**
     * 发送方ID
     */
    private String from;

    /**
     * 时间戳
     */
    private Long timestamp;

    /**
     * SDP 内容 (offer/answer)
     */
    private String sdp;

    /**
     * ICE 候选信息
     */
    private String candidate;

    /**
     * ICE 服务器列表
     */
    private List<IceServer> iceServers;

    /**
     * 自定义数据
     */
    private Object data;

    /**
     * ICE 服务器配置
     */
    @Data
    public static class IceServer {
        private List<String> urls;
        private String username;
        private String credential;
    }

    /**
     * 验证消息是否合法
     */
    public boolean isValid() {
        if (type == null || type.isEmpty()) {
            return false;
        }
        // register 消息只需要 from 字段（WebSocket 会话注册自己的 ID）
        if ("register".equals(type) || "register_response".equals(type)) {
            return from != null && !from.isEmpty() && timestamp != null;
        }
        // ice_request 不需要 target 字段
        if ("ice_request".equals(type)) {
            return timestamp != null;
        }
        // customized 信令 target 字段可选（用于广播场景）
        if ("customized".equals(type)) {
            return timestamp != null;
        }
        // 其他消息需要 target 和 timestamp
        return target != null && !target.isEmpty() && timestamp != null;
    }

    /**
     * 获取响应主题
     */
    public String getDownTopic(String project) {
        return String.format("/voice/%s/%s/down", project, target);
    }
}
