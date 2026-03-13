package com.youkong.audio_server.handler;

import com.alibaba.fastjson.JSON;
import com.youkong.audio_server.dto.SignalingMessage;
import com.youkong.audio_server.service.MqttSignalingService;
import com.youkong.audio_server.service.WebSocketSessionManager;
import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;
import org.springframework.web.socket.CloseStatus;
import org.springframework.web.socket.TextMessage;
import org.springframework.web.socket.WebSocketSession;
import org.springframework.web.socket.handler.TextWebSocketHandler;

import javax.annotation.Resource;
import java.io.IOException;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

/**
 * WebRTC 信令 WebSocket 处理器
 */
@Slf4j
@Component
public class SignalingWebSocketHandler extends TextWebSocketHandler {

    @Resource
    private WebSocketSessionManager sessionManager;

    @Resource
    private MqttSignalingService mqttSignalingService;

    @Value("${ota.server-url:}")
    private String otaServerUrl;

    @Value("${ota.target-version:}")
    private Integer otaTargetVersion;
    @Value("${ota.md5:}")
    private String md5;
    /**
     * 存储 WebSocketSession 与会话ID的映射
     */
    private final ConcurrentMap<WebSocketSession, String> sessionIdMap = new ConcurrentHashMap<>();

    @Override
    public void afterConnectionEstablished(WebSocketSession session) throws Exception {
        String sessionId = session.getId();
        sessionIdMap.put(session, sessionId);
        sessionManager.registerSession(sessionId, session);
        log.info("WebSocket 连接建立: {}, URI: {}", sessionId, session.getUri());

        // 发送连接成功消息
        sendConnectionAck(session);
    }

    @Override
    protected void handleTextMessage(WebSocketSession session, TextMessage message) throws Exception {
        String payload = message.getPayload();
        String sessionId = sessionIdMap.get(session);
        log.info("收到 WebSocket 消息 - 会话: {}, 内容: {}", sessionId, payload);

        try {
            SignalingMessage signalingMessage = JSON.parseObject(payload, SignalingMessage.class);

            // 验证消息格式
            if (!signalingMessage.isValid()) {
                sendErrorMessage(session, "信令格式不合法");
                return;
            }

            // 设置发送方
            if (signalingMessage.getFrom() == null) {
                signalingMessage.setFrom(sessionId);
            }
            signalingMessage.setUrl(otaServerUrl);
            signalingMessage.setVersion(otaTargetVersion);
            signalingMessage.setMd5(md5);

            // 处理不同类型的信令
            handleSignalingMessage(session, signalingMessage);

        } catch (Exception e) {
            log.error("处理 WebSocket 消息失败", e);
            sendErrorMessage(session, "消息解析失败: " + e.getMessage());
        }
    }

    @Override
    public void afterConnectionClosed(WebSocketSession session, CloseStatus status) throws Exception {
        String sessionId = sessionIdMap.remove(session);
        if (sessionId != null) {
            sessionManager.removeSession(sessionId);
            log.info("WebSocket 连接关闭: {}, 状态: {}", sessionId, status);
        }
    }

    @Override
    public void handleTransportError(WebSocketSession session, Throwable exception) throws Exception {
        String sessionId = sessionIdMap.get(session);
        log.error("WebSocket 传输错误 - 会话: {}", sessionId, exception);
    }

    /**
     * 处理信令消息
     */
    private void handleSignalingMessage(WebSocketSession session, SignalingMessage message) throws IOException {
        String type = message.getType();
        String target = message.getTarget();
        String sessionId = sessionIdMap.get(session);

        switch (type) {
            case "register":
                // 注册设备/会话绑定
                handleRegister(session, message);
                break;

            case "offer":
            case "answer":
            case "candidate":
                // WebRTC 信令转发到 ESP32
                forwardToEsp32(sessionId, message);
                break;

            case "bye":
            case "reject":
            case "timeout":
                // 通话终止信令
                handleCallTermination(sessionId, message);
                break;

            case "ice_request":
                // Web 端请求 ICE 凭证
                handleIceRequest(session, message);
                break;

            case "customized":
                // 自定义消息
                forwardToEsp32(sessionId, message);
                break;

            case "version_query":
                // 版本查询请求，转发到 ESP32
                forwardToEsp32(sessionId, message);
                break;

            case "ota":
                // OTA 升级命令，转发到 ESP32
                forwardToEsp32(sessionId, message);
                break;

            default:
                sendErrorMessage(session, "未知的信令类型: " + type);
        }
    }

    /**
     * 处理注册请求
     */
    private void handleRegister(WebSocketSession session, SignalingMessage message) throws IOException {
        String sessionId = sessionIdMap.get(session);
        String deviceId = message.getFrom();

        log.info("处理注册请求 - sessionId: {}, deviceId: {}", sessionId, deviceId);

        if (deviceId == null || deviceId.isEmpty()) {
            sendErrorMessage(session, "注册失败: from 字段不能为空");
            return;
        }

        sessionManager.bindDevice(sessionId, deviceId);

        // 发送注册成功响应
        SignalingMessage response = new SignalingMessage();
        response.setType("register_response");
        response.setTimestamp(System.currentTimeMillis() / 1000);
        response.setData("注册成功: " + deviceId);
        session.sendMessage(new TextMessage(JSON.toJSONString(response)));
    }

    /**
     * 转发信令到 ESP32
     */
    private void forwardToEsp32(String sessionId, SignalingMessage message) {
        String target = message.getTarget();
        if (target == null || target.isEmpty()) {
            log.warn("信令目标为空，无法转发");
            return;
        }

        // 转发到 MQTT
        mqttSignalingService.forwardToEsp32(target, JSON.toJSONString(message));
    }

    /**
     * 处理通话终止
     */
    private void handleCallTermination(String sessionId, SignalingMessage message) {
        String target = message.getTarget();

        // 转发到 MQTT
        mqttSignalingService.forwardToEsp32(target, JSON.toJSONString(message));

        // 通知 Web 端
        sessionManager.sendMessageToDevice(target, JSON.toJSONString(message));
    }

    /**
     * 处理 ICE 凭证请求
     */
    private void handleIceRequest(WebSocketSession session, SignalingMessage request) throws IOException {
        SignalingMessage response = new SignalingMessage();
        response.setType("ice_response");
        response.setTimestamp(System.currentTimeMillis() / 1000);
        response.setIceServers(mqttSignalingService.parseIceServers());

        session.sendMessage(new TextMessage(JSON.toJSONString(response)));
    }

    /**
     * 发送连接确认消息
     */
    private void sendConnectionAck(WebSocketSession session) throws IOException {
        SignalingMessage ack = new SignalingMessage();
        ack.setType("connected");
        ack.setTimestamp(System.currentTimeMillis() / 1000);
        ack.setData("WebSocket 连接成功");
        session.sendMessage(new TextMessage(JSON.toJSONString(ack)));
    }

    /**
     * 发送错误消息
     */
    private void sendErrorMessage(WebSocketSession session, String error) throws IOException {
        SignalingMessage errorMsg = new SignalingMessage();
        errorMsg.setType("error");
        errorMsg.setTimestamp(System.currentTimeMillis() / 1000);
        errorMsg.setData(error);
        session.sendMessage(new TextMessage(JSON.toJSONString(errorMsg)));
    }
}
