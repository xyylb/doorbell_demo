#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WebRTC 测试页面自动化测试脚本
基于 Playwright Python
功能：自动测试 WebSocket 连接功能
"""

import sys
import traceback
from playwright.sync_api import sync_playwright, TimeoutError as PlaywrightTimeoutError


def run_test():
    """
    主测试函数
    运行自动化测试并输出结果
    """
    print("=" * 60)
    print("🚀 WebRTC 测试页面自动化测试")
    print("=" * 60)
    
    browser = None
    playwright = None
    
    try:
        # ============================================================
        # 1. 初始化 Playwright
        # ============================================================
        print("\n[1/6] 正在初始化 Playwright...")
        playwright = sync_playwright().start()
        
        # ============================================================
        # 2. 启动 Chrome 浏览器
        # 参数说明：
        # - headless=False: 显示浏览器窗口（方便调试）
        # - args: 禁用沙盒和安全策略，避免权限问题
        # ============================================================
        print("[2/6] 正在启动 Chrome 浏览器...")
        browser = playwright.chromium.launch(
            headless=False,  # 设置为 True 可无头运行（不显示窗口）
            args=[
                '--no-sandbox',
                '--disable-setuid-sandbox',
                '--disable-dev-shm-usage'
            ]
        )
        
        # 创建新页面（浏览器标签页）
        context = browser.new_context(
            viewport={'width': 1280, 'height': 800}  # 设置窗口大小
        )
        page = context.new_page()
        
        # ============================================================
        # 3. 访问测试页面
        # ============================================================
        print("[3/6] 正在访问 http://localhost:8080 ...")
        page.goto('http://localhost:8080', timeout=30000)  # 30秒超时
        print("✅ 页面加载成功")
        
        # 等待页面完全加载（等待 DOM 就绪）
        page.wait_for_load_state('domcontentloaded')
        
        # ============================================================
        # 4. 定位并点击「连接」按钮
        # 使用 text='连接' 定位蓝色按钮
        # ============================================================
        print("\n[4/6] 正在查找「连接」按钮...")
        
        # 方法1：通过按钮文本定位（推荐，最直观）
        # 页面中蓝色「连接」按钮的文本是"连接"
        connect_button = page.get_by_text('连接').first
        
        # 验证按钮是否存在且可见
        if not connect_button.is_visible():
            raise Exception("「连接」按钮不可见")
        
        print("✅ 找到「连接」按钮")
        
        # 点击按钮
        print("🖱️  正在点击「连接」按钮...")
        connect_button.click()
        
        # ============================================================
        # 5. 等待 WebSocket 连接建立
        # 等待 2 秒让连接完成
        # ============================================================
        print("\n[5/6] 等待 WebSocket 连接建立（2秒）...")
        page.wait_for_timeout(2000)  # 等待 2000 毫秒（2秒）
        
        # ============================================================
        # 6. 验证连接状态
        # 查找 class='status' 的元素，验证文本是否为「已连接」
        # ============================================================
        print("\n[6/6] 正在验证连接状态...")
        
        # 定位状态元素（class='status'）
        # 使用 CSS 选择器定位
        status_element = page.locator('.status').first
        
        # 等待元素可见（最多等待 5 秒）
        status_element.wait_for(state='visible', timeout=5000)
        
        # 获取状态文本
        status_text = status_element.inner_text()
        print(f"📊 当前连接状态: 「{status_text}」")
        
        # ============================================================
        # 验证结果
        # ============================================================
        print("\n" + "=" * 60)
        if status_text == '已连接':
            print("✅ 测试通过！WebSocket 连接成功")
            print("=" * 60)
            return True
        else:
            print(f"❌ 测试失败！期望状态「已连接」，实际状态「{status_text}」")
            print("=" * 60)
            return False
            
    except PlaywrightTimeoutError as e:
        # 处理 Playwright 超时异常
        print(f"\n❌ 超时错误: {str(e)}")
        print("可能原因：页面加载缓慢或元素未找到")
        traceback.print_exc()
        return False
        
    except Exception as e:
        # 处理其他异常
        print(f"\n❌ 测试执行出错: {str(e)}")
        traceback.print_exc()
        return False
        
    finally:
        # ============================================================
        # 清理资源
        # 无论测试成功与否，都要关闭浏览器
        # ============================================================
        print("\n🧹 正在清理资源...")
        if browser:
            browser.close()
            print("✅ 浏览器已关闭")
        if playwright:
            playwright.stop()
            print("✅ Playwright 已停止")


def check_dependencies():
    """
    检查依赖是否安装
    如果没有安装 playwright，提示用户安装
    """
    try:
        import playwright
        return True
    except ImportError:
        print("❌ 未检测到 Playwright 库")
        print("\n请先在 Trae 终端中安装依赖：")
        print("  pip install playwright")
        print("  playwright install chromium")
        return False


if __name__ == '__main__':
    # 检查依赖
    if not check_dependencies():
        sys.exit(1)
    
    # 运行测试
    success = run_test()
    
    # 根据测试结果设置退出码
    # 0 表示成功，1 表示失败（CI/CD 常用）
    sys.exit(0 if success else 1)
