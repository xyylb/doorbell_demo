package com.youkong.audio_server.service;

import lombok.extern.slf4j.Slf4j;
import org.springframework.stereotype.Service;
import org.springframework.web.socket.TextMessage;
import org.springframework.web.socket.WebSocketSession;

import java.io.IOException;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/**
 * WebSocket 会话管理器
 */
@Slf4j
@Service
public class WebSocketSessionManager {

    /**
     * 会话映射表: sessionId -> WebSocketSession
     */
    private final Map<String, WebSocketSession> sessions = new ConcurrentHashMap<>();

    /**
     * 设备与会话的映射: deviceId -> sessionId
     */
    private final Map<String, String> deviceSessionMap = new ConcurrentHashMap<>();

    /**
     * 会话与设备的映射: sessionId -> deviceId
     */
    private final Map<String, String> sessionDeviceMap = new ConcurrentHashMap<>();

    private final ScheduledExecutorService heartbeatExecutor = Executors.newSingleThreadScheduledExecutor();

    public WebSocketSessionManager() {
        // 启动定时清理任务
        heartbeatExecutor.scheduleAtFixedRate(this::cleanupInactiveSessions, 30, 30, TimeUnit.SECONDS);
    }

    /**
     * 注册会话
     */
    public void registerSession(String sessionId, WebSocketSession session) {
        sessions.put(sessionId, session);
        log.info("WebSocket 会话注册成功: {}, 当前会话数: {}", sessionId, sessions.size());
    }

    /**
     * 绑定设备与会话
     */
    public void bindDevice(String sessionId, String deviceId) {
        deviceSessionMap.put(deviceId, sessionId);
        sessionDeviceMap.put(sessionId, deviceId);
        log.info("绑定设备与会话: deviceId={}, sessionId={}", deviceId, sessionId);
    }

    /**
     * 解绑设备与会话
     */
    public void unbindDevice(String sessionId) {
        String deviceId = sessionDeviceMap.remove(sessionId);
        if (deviceId != null) {
            deviceSessionMap.remove(deviceId);
            log.info("解绑设备与会话: deviceId={}, sessionId={}", deviceId, sessionId);
        }
    }

    /**
     * 获取绑定的设备ID
     */
    public String getDeviceId(String sessionId) {
        return sessionDeviceMap.get(sessionId);
    }

    /**
     * 移除会话
     */
    public void removeSession(String sessionId) {
        sessions.remove(sessionId);
        unbindDevice(sessionId);
        log.info("WebSocket 会话移除: {}, 当前会话数: {}", sessionId, sessions.size());
    }

    /**
     * 发送消息到指定会话
     */
    public boolean sendMessageToSession(String sessionId, String message) {
        WebSocketSession session = sessions.get(sessionId);
        if (session != null && session.isOpen()) {
            try {
                session.sendMessage(new TextMessage(message));
                log.debug("发送消息到会话 {}: {}", sessionId, message);
                return true;
            } catch (IOException e) {
                log.error("发送消息到会话 {} 失败", sessionId, e);
            }
        } else {
            log.warn("会话 {} 不存在或已关闭", sessionId);
        }
        return false;
    }

    /**
     * 发送消息到指定设备
     */
    public boolean sendMessageToDevice(String deviceId, String message) {
        String sessionId = deviceSessionMap.get(deviceId);
        if (sessionId != null) {
            return sendMessageToSession(sessionId, message);
        }
        log.warn("设备 {} 未绑定到任何会话", deviceId);
        return false;
    }

    /**
     * 广播消息到所有会话
     */
    public void broadcastMessage(String message) {
        sessions.forEach((sessionId, session) -> {
            if (session.isOpen()) {
                try {
                    session.sendMessage(new TextMessage(message));
                } catch (IOException e) {
                    log.error("广播消息到会话 {} 失败", sessionId, e);
                }
            }
        });
    }

    /**
     * 获取当前会话数
     */
    public int getSessionCount() {
        return sessions.size();
    }

    /**
     * 清理不活跃的会话
     */
    private void cleanupInactiveSessions() {
        sessions.entrySet().removeIf(entry -> {
            WebSocketSession session = entry.getValue();
            if (!session.isOpen()) {
                String sessionId = entry.getKey();
                unbindDevice(sessionId);
                log.info("清理不活跃会话: {}", sessionId);
                return true;
            }
            return false;
        });
    }

    /**
     * 检查会话是否存在
     */
    public boolean hasSession(String sessionId) {
        WebSocketSession session = sessions.get(sessionId);
        return session != null && session.isOpen();
    }
}
