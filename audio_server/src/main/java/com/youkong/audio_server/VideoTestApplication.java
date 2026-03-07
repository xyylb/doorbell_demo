package com.youkong.audio_server;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.scheduling.annotation.EnableScheduling;

@EnableScheduling
@SpringBootApplication
public class VideoTestApplication {

	public static void main(String[] args) {
		SpringApplication.run(VideoTestApplication.class, args);
	}
}
