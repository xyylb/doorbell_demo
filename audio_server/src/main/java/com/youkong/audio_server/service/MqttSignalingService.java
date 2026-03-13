package com.youkong.audio_server.service;

import com.alibaba.fastjson.JSON;
import com.alibaba.fastjson.JSONObject;
import com.youkong.audio_server.dto.SignalingMessage;
import lombok.Data;
import lombok.extern.slf4j.Slf4j;
import org.eclipse.paho.client.mqttv3.MqttClient;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.integration.support.MessageBuilder;
import org.springframework.stereotype.Service;

import javax.annotation.PostConstruct;
import javax.annotation.Resource;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

/**
 * MQTT 信令服务
 * 处理 ESP32 设备与 WebSocket 之间的信令转发
 */
@Slf4j
@Service
public class MqttSignalingService {

    private MqttClient mqttClient;

    @Value("${spring.mqtt.broker-url:tcp://visit-repair.2811.top:1883}")
    private String brokerUrl;

    @Value("${spring.mqtt.client-id:mqttSignalingService}")
    private String clientId;

    @Value("${spring.mqtt.qos:1}")
    private int qos;

    @Value("${signaling.project:project}")
    private String project;

    @Value("${signaling.persistence.enabled:true}")
    private boolean persistenceEnabled;

    @Value("${signaling.persistence.file:resources/signaling.log}")
    private String persistenceFile;

    /**
     * WebSocket 会话管理器
     */
    @Resource
    private WebSocketSessionManager sessionManager;

    /**
     * ICE 服务器配置
     */
    @Value("${ice.servers:}")
    private String iceServersConfig;

    private File logFile;
    private final Object logLock = new Object();

    private final ConcurrentMap<String, DeviceVersion> deviceVersions = new ConcurrentHashMap<>();

    @Value("${ota.server-url:}")
    private String otaServerUrl;

    @Value("${ota.target-version:}")
    private Integer otaTargetVersion;

    /**
     * 通话状态管理
     */
    private final ConcurrentMap<String, CallSession> callSessions = new ConcurrentHashMap<>();

    @PostConstruct
    public void init() {
        initMqttClient();
        if (persistenceEnabled) {
            initPersistenceFile();
        }
    }

    /**
     * 初始化 MQTT 客户端（用于发送消息）
     */
    private void initMqttClient() {
        try {
            mqttClient = new MqttClient(brokerUrl, clientId + "-publisher", null);
            mqttClient.connect();
            log.info("MQTT 发布客户端初始化成功");
        } catch (MqttException e) {
            log.error("MQTT 发布客户端初始化失败: {}", e.getMessage());
        }
    }

    /**
     * 初始化信令持久化文件
     */
    private void initPersistenceFile() {
        try {
            logFile = new File(persistenceFile);
            File parentDir = logFile.getParentFile();
            if (parentDir != null && !parentDir.exists()) {
                parentDir.mkdirs();
            }
            if (!logFile.exists()) {
                logFile.createNewFile();
            }
            log.info("信令持久化文件初始化完成: {}", logFile.getAbsolutePath());
        } catch (IOException e) {
            log.error("初始化信令持久化文件失败", e);
        }
    }

    /**
     * 处理从 ESP32 接收到的 MQTT 消息
     */
    public void handleMqttMessage(String topic, String payload) {
        log.info("处理 MQTT 上行信令 - 主题: {}, 内容: {}", topic, payload);

        // 持久化信令
        persistSignaling("UP", topic, payload);

        try {
            SignalingMessage message = JSON.parseObject(payload, SignalingMessage.class);

            // 验证消息格式
            if (!message.isValid()) {
                log.warn("信令格式不合法: {}", payload);
                return;
            }

            String type = message.getType();

            switch (type) {
                case "ice_request":
                    handleIceRequest(topic, message);
                    break;
                case "offer":
                case "answer":
                case "candidate":
                    handleWebRtcSignaling(message);
                    break;
                case "bye":
                case "reject":
                case "timeout":
                    handleCallTermination(message);
                    break;
                case "customized":
                    handleCustomizedMessage(message);
                    break;

                case "ring":
                    // 自定义消息
                    handleCustomizedMessage(message);
                    break;
                case "version_report":
                    handleVersionReport(message);
                    break;
                default:
                    log.warn("未知的信令类型: {}", type);
            }

        } catch (Exception e) {
            log.error("处理 MQTT 消息失败", e);
        }
    }

    /**
     * 处理 ICE 凭证请求
     */
    private void handleIceRequest(String topic, SignalingMessage request) {
        log.info("处理 ICE 凭证请求");

        // 从主题中提取设备ID
        String deviceId = extractDeviceIdFromTopic(topic);
        if (deviceId == null) {
            log.error("无法从主题提取设备ID: {}", topic);
            return;
        }

        // 构建 ICE 响应
        SignalingMessage response = new SignalingMessage();
        response.setType("ice_response");
        response.setTimestamp(System.currentTimeMillis() / 1000);

        // 解析 ICE 服务器配置
        response.setIceServers(parseIceServers());

        // 发送响应到设备下行主题
        String downTopic = String.format("/voice/%s/%s/down", project, deviceId);
        sendMqttMessage(downTopic, JSON.toJSONString(response));
    }

    private void handleVersionReport(SignalingMessage message) {
        String deviceId = message.getFrom();
        Integer version = message.getVersion();

        if (deviceId == null || version == null) {
            log.warn("版本上报信息不完整");
            return;
        }

        log.info("收到设备版本上报: deviceId={}, version={}", deviceId, version);

        DeviceVersion dv = new DeviceVersion();
        dv.setDeviceId(deviceId);
        dv.setVersion(version);
        dv.setLastUpdateTime(System.currentTimeMillis());
        deviceVersions.put(deviceId, dv);

        String target = message.getTarget();
        // 如果 target 为空，则广播给所有会话
        if (target != null && !target.isEmpty()) {
            // 告诉web端版本
            sessionManager.sendMessageToDevice(target, JSON.toJSONString(message));
        }
        log.info("设备版本已记录: deviceId={}, version={}", deviceId, version);
    }

    public void manualOtaUpgrade(String deviceId, Integer version, String url) {
        if (deviceId == null || deviceId.isEmpty()) {
            log.warn("设备ID为空");
            return;
        }

        if (url == null || url.isEmpty()) {
            log.warn("OTA URL为空");
            return;
        }

        log.info("手动推送 OTA 升级到设备 {}: URL={}, 版本={}", deviceId, url, version);

        SignalingMessage otaMsg = new SignalingMessage();
        otaMsg.setType("ota");
        otaMsg.setTimestamp(System.currentTimeMillis() / 1000);
        otaMsg.setTarget(deviceId);
        otaMsg.setVersion(version != null ? version : 0);
        otaMsg.setUrl(url);
        otaMsg.setMd5("");

        String downTopic = String.format("/voice/%s/%s/down", project, deviceId);
        sendMqttMessage(downTopic, JSON.toJSONString(otaMsg));

        log.info("已发送 OTA 升级消息到设备 {}", deviceId);
    }

    public void queryVersion(String deviceId) {
        log.info("查询设备 {} 版本", deviceId);

        SignalingMessage msg = new SignalingMessage();
        msg.setType("version_query");
        msg.setTimestamp(System.currentTimeMillis() / 1000);
        msg.setTarget(deviceId);

        String downTopic = String.format("/voice/%s/%s/down", project, deviceId);
        sendMqttMessage(downTopic, JSON.toJSONString(msg));
    }

    /**
     * 解析 ICE 服务器配置
     */
    public java.util.List<SignalingMessage.IceServer> parseIceServers() {
        java.util.List<SignalingMessage.IceServer> servers = new java.util.ArrayList<>();

        // 默认使用免费的 STUN 服务器
        if (iceServersConfig == null || iceServersConfig.isEmpty()) {
            SignalingMessage.IceServer stunServer = new SignalingMessage.IceServer();
            stunServer.setUrls(java.util.Arrays.asList(
                "stun:turn.vcall.75prc.cn:3478",
                "turn:turn.vcall.75prc.cn:3478"
            ));
            stunServer.setUsername("youkongcall");
            stunServer.setCredential("HJbnf23oNF23BgdlNM");
            servers.add(stunServer);
            return servers;
        }

        // 解析配置的 ICE 服务器
        try {
            String[] configs = iceServersConfig.split(";");
            for (String config : configs) {
                String[] parts = config.split(",");
                SignalingMessage.IceServer server = new SignalingMessage.IceServer();
                server.setUrls(java.util.Arrays.asList(parts[0].trim()));
                if (parts.length > 1) {
                    server.setUsername(parts[1].trim());
                }
                if (parts.length > 2) {
                    server.setCredential(parts[2].trim());
                }
                servers.add(server);
            }
        } catch (Exception e) {
            log.error("解析 ICE 服务器配置失败", e);
        }

        return servers;
    }

    /**
     * 处理 WebRTC 信令（offer/answer/candidate）
     */
    private void handleWebRtcSignaling(SignalingMessage message) {
        String target = message.getTarget();

        // 更新通话状态
        if ("offer".equals(message.getType())) {
            createCallSession(message.getFrom(), target);
        }

        // 转发到 WebSocket - 使用 sendMessageToDevice 根据设备ID查找会话
        sessionManager.sendMessageToDevice(target, JSON.toJSONString(message));
    }

    /**
     * 处理通话终止信令
     */
    private void handleCallTermination(SignalingMessage message) {
        String target = message.getTarget();

        // 清理通话会话
        terminateCallSession(target);

        // 转发到 WebSocket - 使用 sendMessageToDevice 根据设备ID查找会话
        sessionManager.sendMessageToDevice(target, JSON.toJSONString(message));
    }

    /**
     * 处理自定义消息
     */
    private void handleCustomizedMessage(SignalingMessage message) {
        String target = message.getTarget();
        // 如果 target 为空，则广播给所有会话
        if (target == null || target.isEmpty()) {
            sessionManager.broadcastMessage(JSON.toJSONString(message));
            log.info("广播 customized 消息到所有 WebSocket 会话");
        } else {
            // 使用 sendMessageToDevice 根据设备ID查找会话
            sessionManager.sendMessageToDevice(target, JSON.toJSONString(message));
        }
    }

    /**
     * 发送 MQTT 消息到 ESP32
     */
    public void sendMqttMessage(String topic, String payload) {
        log.info("发送 MQTT 下行信令 - 主题: {}, 内容: {}", topic, payload);

        try {
            if (mqttClient != null && mqttClient.isConnected()) {
                MqttMessage message = new MqttMessage(payload.getBytes());
                message.setQos(qos);
                mqttClient.publish(topic, message);
            } else {
                log.warn("MQTT 客户端未连接，无法发送消息");
            }
        } catch (MqttException e) {
            log.error("发送 MQTT 消息失败: {}", e.getMessage());
        }

        // 持久化下行信令
        persistSignaling("DOWN", topic, payload);
    }

    /**
     * 从 WebSocket 转发信令到 ESP32
     */
    public void forwardToEsp32(String deviceId, String payload) {
        String downTopic = String.format("/voice/%s/%s/down", project, deviceId);
        sendMqttMessage(downTopic, payload);
    }

    /**
     * 从主题中提取设备ID
     */
    private String extractDeviceIdFromTopic(String topic) {
        // 主题格式: /voice/project/{deviceId}/up
        String[] parts = topic.split("/");
        if (parts.length >= 4) {
            return parts[parts.length - 2];
        }
        return null;
    }

    /**
     * 持久化信令到文件
     */
    private void persistSignaling(String direction, String topic, String payload) {
        if (!persistenceEnabled || logFile == null) {
            return;
        }

        synchronized (logLock) {
            try (PrintWriter writer = new PrintWriter(new FileWriter(logFile, true))) {
                String timestamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"));
                writer.println(String.format("[%s] [%s] Topic: %s", timestamp, direction, topic));
                writer.println("Payload: " + payload);
                writer.println("---");
            } catch (IOException e) {
                log.error("持久化信令失败", e);
            }
        }
    }

    /**
     * 创建通话会话
     */
    private void createCallSession(String from, String to) {
        String sessionId = generateSessionId(from, to);
        CallSession session = new CallSession();
        session.setSessionId(sessionId);
        session.setCaller(from);
        session.setCallee(to);
        session.setStatus(CallStatus.CALLING);
        session.setStartTime(System.currentTimeMillis());

        callSessions.put(sessionId, session);
        log.info("创建通话会话: {}", sessionId);
    }

    /**
     * 终止通话会话
     */
    private void terminateCallSession(String deviceId) {
        callSessions.forEach((sessionId, session) -> {
            if (session.getCaller().equals(deviceId) || session.getCallee().equals(deviceId)) {
                session.setStatus(CallStatus.ENDED);
                session.setEndTime(System.currentTimeMillis());
                log.info("终止通话会话: {}", sessionId);
            }
        });
    }

    /**
     * 生成会话ID
     */
    private String generateSessionId(String from, String to) {
        return from + "_" + to + "_" + System.currentTimeMillis();
    }

    /**
     * 通话状态枚举
     */
    public enum CallStatus {
        CALLING,    // 呼叫中
        CONNECTED,  // 通话中
        ENDED       // 已结束
    }

    /**
     * 通话会话
     */
    @Data
    public static class CallSession {
        private String sessionId;
        private String caller;
        private String callee;
        private CallStatus status;
        private long startTime;
        private long endTime;
    }

    @Data
    public static class DeviceVersion {
        private String deviceId;
        private Integer version;
        private long lastUpdateTime;
    }
}
