package com.youkong.audio_server.util;

import org.springframework.integration.support.MessageBuilder;
import org.springframework.messaging.MessageChannel;
import org.springframework.stereotype.Component;

import javax.annotation.Resource;

/**
 * MQTT 消息发送工具类
 */
@Component
public class MqttSender {

    @Resource(name = "mqttOutputChannel")
    private MessageChannel mqttOutputChannel;

    /**
     * 发送MQTT消息（使用默认主题）
     * @param payload 消息内容
     */
    public void send(String payload) {
        mqttOutputChannel.send(MessageBuilder.withPayload(payload).build());
    }

    /**
     * 发送MQTT消息（指定主题）
     * @param topic 主题
     * @param payload 消息内容
     */
    public void send(String topic, String payload) {
        mqttOutputChannel.send(MessageBuilder.withPayload(payload)
                .setHeader("mqtt_topic", topic)
                .build());
    }

    /**
     * 发送MQTT消息（指定主题和QoS）
     * @param topic 主题
     * @param qos QoS级别 0/1/2
     * @param payload 消息内容
     */
    public void send(String topic, int qos, String payload) {
        mqttOutputChannel.send(MessageBuilder.withPayload(payload)
                .setHeader("mqtt_topic", topic)
                .setHeader("mqtt_qos", qos)
                .build());
    }
}
