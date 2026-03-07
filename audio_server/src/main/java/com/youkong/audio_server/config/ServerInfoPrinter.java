package com.youkong.audio_server.config;

import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.CommandLineRunner;
import org.springframework.stereotype.Component;

import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Enumeration;

/**
 * 服务器启动信息打印
 */
@Slf4j
@Component
public class ServerInfoPrinter implements CommandLineRunner {

    @Value("${server.port:8080}")
    private int serverPort;

    @Override
    public void run(String... args) throws Exception {
        log.info("==================================================");
        log.info("🚀 WebRTC 信令服务器启动成功!");
        log.info("==================================================");
        
        // 获取本机IP地址
        String ip = getLocalIpAddress();
        
        log.info("📡 访问地址:");
        log.info("   本机访问: http://localhost:{}", serverPort);
        log.info("   局域网访问: http://{}:{}", ip, serverPort);
        log.info("   WebSocket: ws://{}:{}/signaling", ip, serverPort);
        
        log.info("--------------------------------------------------");
        log.info("📋 功能说明:");
        log.info("   - MQTT 信令转发: /voice/project/{deviceId}/up/down");
        log.info("   - WebSocket 信令: /signaling");
        log.info("   - 测试页面: http://{}:{}/index.html", ip, serverPort);
        log.info("==================================================");
    }

    /**
     * 获取本机IP地址
     */
    private String getLocalIpAddress() {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces.hasMoreElements()) {
                NetworkInterface networkInterface = interfaces.nextElement();
                // 跳过回环接口和虚拟接口
                if (networkInterface.isLoopback() || networkInterface.isVirtual() || !networkInterface.isUp()) {
                    continue;
                }
                
                Enumeration<InetAddress> addresses = networkInterface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress addr = addresses.nextElement();
                    // 只获取IPv4地址
                    if (!addr.isLoopbackAddress() && addr.getHostAddress().indexOf(':') == -1) {
                        return addr.getHostAddress();
                    }
                }
            }
        } catch (Exception e) {
            log.warn("获取本机IP地址失败", e);
        }
        return "127.0.0.1";
    }
}
