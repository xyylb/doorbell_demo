package com.youkong.audio_server.controller;


import com.youkong.audio_server.util.MqttSender;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import javax.annotation.Resource;

/**
 * MQTT 测试控制器
 */
@RestController
@RequestMapping("/mqtt")
public class MqttTestController {

    @Resource
    private MqttSender mqttSender;

    /**
     * 发送MQTT消息
     * @param topic 主题（可选，默认使用配置文件中的主题）
     * @param message 消息内容
     * @return 发送结果
     */
    @GetMapping("/send")
    public String sendMessage(
            @RequestParam(required = false) String topic,
            @RequestParam String message) {
        if (topic == null || topic.isEmpty()) {
            mqttSender.send(message);
            return "使用默认主题发送消息成功: " + message;
        } else {
            mqttSender.send(topic, message);
            return "发送消息到主题 [" + topic + "] 成功: " + message;
        }
    }
}
