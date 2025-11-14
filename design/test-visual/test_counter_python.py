#!/usr/bin/env python3
"""
SystemC Counter 模組 Python 測試腳本
這個腳本演示如何使用 pybind11 包裝的 SystemC Counter 模組
"""

import counter_systemc as cs

def run_all_tests():
    """運行所有測試，使用單一測試台實例"""
    print("SystemC Counter Python 測試")
    print("=" * 50)

    # 創建單一測試台實例
    tb = cs.CounterTestBench("main_tb")

    # 初始化 SystemC
    cs.SystemCSimulation.initialize()

    # 測試 1: 基本計數器測試
    print("=== 測試 1: 基本計數器功能 ===")

    print(f"初始時間: {tb.get_current_time():.1f} ns")
    print(f"初始計數值: {tb.get_count_out()}")

    # 重置計數器
    print("\n1. 重置計數器")
    tb.reset_counter()
    print(f"重置後計數值: {tb.get_count_out()}")
    print(f"當前時間: {tb.get_current_time():.1f} ns")

    # 增加計數器
    print("\n2. 增加計數器 5 次")
    for i in range(5):
        tb.increase_counter()
        print(f"  增加 {i+1}: 計數值 = {tb.get_count_out()}, 時間 = {tb.get_current_time():.1f} ns")

    # 設置計數器值
    print("\n3. 設置計數器為 100")
    tb.set_counter_value(100)
    print(f"設置後計數值: {tb.get_count_out()}")
    print(f"溢出狀態: {'是' if tb.get_overflow() else '否'}")

    # 清零計數器
    print("\n4. 清零計數器")
    tb.clear_counter()
    print(f"清零後計數值: {tb.get_count_out()}")

    # 測試 2: 溢出測試
    print("\n=== 測試 2: 溢出功能 ===")

    # 設置接近最大值
    print("設置計數器為 998")
    tb.set_counter_value(998)
    print(f"設置後計數值: {tb.get_count_out()}")
    print(f"溢出狀態: {'是' if tb.get_overflow() else '否'}")

    # 增加到最大值
    print("\n增加到最大值 (1000)")
    tb.increase_counter()  # 999
    tb.increase_counter()  # 1000
    print(f"計數值: {tb.get_count_out()}")
    print(f"溢出狀態: {'是' if tb.get_overflow() else '否'}")

    # 嘗試再次增加
    print("\n嘗試繼續增加")
    tb.increase_counter()
    print(f"計數值: {tb.get_count_out()}")
    print(f"溢出狀態: {'是' if tb.get_overflow() else '否'}")

    # 測試 3: 手動控制信號
    print("\n=== 測試 3: 手動控制信號 ===")

    # 重置計數器到乾淨狀態
    tb.reset_counter()

    # 手動控制信號
    print("手動設置信號並運行時鐘週期")

    # 設置增加信號並運行幾個週期
    tb.set_increase_signal(True)
    tb.step_clock(1)
    print(f"增加信號激活後: {tb.get_count_out()}")

    tb.set_increase_signal(False)
    tb.step_clock(1)
    print(f"增加信號停止後: {tb.get_count_out()}")

    # 設置特定值
    tb.set_set_value(50)
    tb.set_set_signal(True)
    tb.step_clock(1)
    print(f"設置為50後: {tb.get_count_out()}")

    tb.set_set_signal(False)
    tb.step_clock(1)

    # 測試 4: 運行時間測試
    print(f"\n=== 測試 4: 時間控制 ===")
    current_time = tb.get_current_time()
    print(f"當前時間: {current_time:.1f} ns")

    # 運行一段時間
    print("運行 50ns 模擬")
    tb.run_simulation(50)
    print(f"模擬結束時間: {tb.get_current_time():.1f} ns")

    print(f"\n最終計數值: {tb.get_count_out()}")
    print(f"最終時間: {tb.get_current_time():.1f} ns")

if __name__ == "__main__":
    try:
        run_all_tests()

        print("\n" + "=" * 50)
        print("所有測試完成！")

    except Exception as e:
        print(f"測試過程中出現錯誤: {e}")
        import traceback
        traceback.print_exc()