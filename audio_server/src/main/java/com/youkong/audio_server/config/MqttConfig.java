package com.youkong.audio_server.config;

import com.youkong.audio_server.service.MqttSignalingService;
import lombok.extern.slf4j.Slf4j;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.integration.annotation.ServiceActivator;
import org.springframework.integration.channel.DirectChannel;
import org.springframework.integration.core.MessageProducer;
import org.springframework.integration.mqtt.core.DefaultMqttPahoClientFactory;
import org.springframework.integration.mqtt.core.MqttPahoClientFactory;
import org.springframework.integration.mqtt.inbound.MqttPahoMessageDrivenChannelAdapter;
import org.springframework.integration.mqtt.outbound.MqttPahoMessageHandler;
import org.springframework.integration.mqtt.support.DefaultPahoMessageConverter;
import org.springframework.messaging.MessageChannel;
import org.springframework.messaging.MessageHandler;

import javax.annotation.Resource;

/**
 * MQTT 客户端配置类
 */
@Slf4j
@Configuration
public class MqttConfig {

    @Resource
    private MqttSignalingService mqttSignalingService;

    @Value("${spring.mqtt.broker-url}")
    private String mqttUrl;

    @Value("${spring.mqtt.client-id}")
    private String clientId;

    @Value("${spring.mqtt.username}")
    private String username;

    @Value("${spring.mqtt.password}")
    private String password;

    @Value("${spring.mqtt.default-topics}")
    private String defaultTopic;

    @Value("${spring.mqtt.connection-timeout}")
    private int connectionTimeout;

    @Value("${spring.mqtt.keep-alive-interval}")
    private int keepAliveInterval;

    @Value("${spring.mqtt.clean-session}")
    private boolean cleanSession;

    @Value("${spring.mqtt.qos}")
    private int qos;

    @Value("${signaling.project:project}")
    private String project;

    /**
     * MQTT 连接配置
     */
    @Bean
    public MqttConnectOptions mqttConnectOptions() {
        MqttConnectOptions options = new MqttConnectOptions();
        // 设置服务器地址
        options.setServerURIs(mqttUrl.split(","));
        // 设置用户名密码
        if (username != null && !username.isEmpty()) {
            options.setUserName(username);
        }
        if (password != null && !password.isEmpty()) {
            options.setPassword(password.toCharArray());
        }
        // 连接超时
        options.setConnectionTimeout(connectionTimeout);
        // 心跳间隔
        options.setKeepAliveInterval(keepAliveInterval);
        // 清除会话
        options.setCleanSession(cleanSession);
        // 自动重连
        options.setAutomaticReconnect(true);
        return options;
    }

    /**
     * MQTT 客户端工厂
     */
    @Bean
    public MqttPahoClientFactory mqttClientFactory() {
        DefaultMqttPahoClientFactory factory = new DefaultMqttPahoClientFactory();
        factory.setConnectionOptions(mqttConnectOptions());
        return factory;
    }

    /**
     * 入站通道（接收MQTT消息）
     */
    @Bean(name = "mqttInputChannel")
    public MessageChannel mqttInputChannel() {
        return new DirectChannel();
    }

    /**
     * 出站通道（发送MQTT消息）
     */
    @Bean(name = "mqttOutputChannel")
    public MessageChannel mqttOutputChannel() {
        return new DirectChannel();
    }

    /**
     * MQTT 消息生产者（订阅者）
     */
    @Bean
    public MessageProducer inbound() {
        // 订阅 ESP32 设备上行主题
        String upTopic = "/voice/" + project + "/+/up";

        // 创建消息驱动的通道适配器
        MqttPahoMessageDrivenChannelAdapter adapter = new MqttPahoMessageDrivenChannelAdapter(
                clientId + "-inbound", // 入站客户端ID，避免和出站重复
                mqttClientFactory(),
                upTopic); // 使用通配符订阅所有设备的上行主题

        // 设置消息转换器（UTF-8编码，QoS级别）
        adapter.setConverter(new DefaultPahoMessageConverter(qos, false));
        adapter.setQos(qos);
        // 设置消息接收通道
        adapter.setOutputChannel(mqttInputChannel());
        log.info("MQTT 订阅主题: {}", upTopic);
        return adapter;
    }

    /**
     * 处理接收到的MQTT消息
     */
    @Bean
    @ServiceActivator(inputChannel = "mqttInputChannel")
    public MessageHandler handler() {
        return message -> {
            String topic = message.getHeaders().get("mqtt_receivedTopic").toString();
            String payload = message.getPayload().toString();
            log.info("收到MQTT消息 - 主题: {}, 内容: {}", topic, payload);
            // 转发到信令服务处理
            mqttSignalingService.handleMqttMessage(topic, payload);
        };
    }

    /**
     * MQTT 消息处理器（发布者）
     */
    @Bean
    @ServiceActivator(inputChannel = "mqttOutputChannel")
    public MessageHandler mqttOutbound() {
        MqttPahoMessageHandler messageHandler = new MqttPahoMessageHandler(
                clientId + "-outbound", mqttClientFactory());
        // 设置默认QoS
        messageHandler.setDefaultQos(qos);
        // 异步发送
        messageHandler.setAsync(true);
        // 设置默认主题
        messageHandler.setDefaultTopic(defaultTopic);
        return messageHandler;
    }
}
