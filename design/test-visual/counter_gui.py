#!/usr/bin/env python3
"""
SystemC Counter 視覺化監控界面
使用 PyQt5 創建的 GUI 應用程式來控制和監視 SystemC Counter 模組
"""

import sys
import os
from PyQt5.QtWidgets import (QApplication, QMainWindow, QVBoxLayout, QHBoxLayout,
                             QWidget, QPushButton, QLabel, QLineEdit, QTextEdit,
                             QGroupBox, QGridLayout, QProgressBar, QSlider,
                             QCheckBox, QSpinBox, QFrame, QSplitter)
from PyQt5.QtCore import QTimer, Qt, pyqtSignal, QThread, pyqtSlot
from PyQt5.QtGui import QFont, QPalette, QColor, QIcon

# 添加當前目錄到 Python 路徑以便導入 counter_systemc
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import counter_systemc as cs
except ImportError as e:
    print(f"錯誤：無法導入 counter_systemc 模組: {e}")
    print("請確保已經構建了 Python 擴展模組")
    sys.exit(1)

class CounterSimulationWorker(QThread):
    """SystemC 模擬工作線程"""
    update_signal = pyqtSignal(dict)

    def __init__(self, testbench):
        super().__init__()
        self.testbench = testbench
        self.running = False
        self.auto_mode = False
        self.step_interval = 100  # ms

    def run(self):
        self.running = True
        while self.running:
            if self.auto_mode:
                # 自動模式：自動增加計數器
                self.testbench.increase_counter()

            # 發送更新信號
            data = {
                'count': self.testbench.get_count_out(),
                'overflow': self.testbench.get_overflow(),
                'time': self.testbench.get_current_time()
            }
            self.update_signal.emit(data)

            # 等待指定間隔
            self.msleep(self.step_interval)

    def stop(self):
        self.running = False
        self.wait()

class CounterMonitorWidget(QMainWindow):
    """SystemC Counter 監控主窗口"""

    def __init__(self):
        super().__init__()
        self.init_systemc()
        self.init_ui()
        self.init_timer()

    def init_systemc(self):
        """初始化 SystemC 測試台"""
        try:
            self.testbench = cs.CounterTestBench("gui_tb")
            cs.SystemCSimulation.initialize()
            self.simulation_worker = CounterSimulationWorker(self.testbench)
            self.simulation_worker.update_signal.connect(self.update_display)
        except Exception as e:
            print(f"SystemC 初始化失敗: {e}")
            sys.exit(1)

    def init_ui(self):
        """初始化用戶界面"""
        self.setWindowTitle("SystemC Counter 視覺化監控器")
        self.setGeometry(100, 100, 1000, 700)

        # 創建主窗口部件
        main_widget = QWidget()
        self.setCentralWidget(main_widget)

        # 創建主佈局
        main_layout = QHBoxLayout()
        main_widget.setLayout(main_layout)

        # 創建分割器
        splitter = QSplitter(Qt.Horizontal)
        main_layout.addWidget(splitter)

        # 左側控制面板
        control_panel = self.create_control_panel()
        splitter.addWidget(control_panel)

        # 右側顯示面板
        display_panel = self.create_display_panel()
        splitter.addWidget(display_panel)

        # 設置分割比例
        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 2)

        # 設置樣式
        self.setStyleSheet("""
            QMainWindow {
                background-color: #f0f0f0;
            }
            QGroupBox {
                font-weight: bold;
                border: 2px solid #cccccc;
                border-radius: 5px;
                margin-top: 10px;
                padding-top: 10px;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px 0 5px;
            }
            QPushButton {
                background-color: #4CAF50;
                border: none;
                color: white;
                padding: 8px 16px;
                text-align: center;
                text-decoration: none;
                display: inline-block;
                font-size: 14px;
                margin: 4px 2px;
                border-radius: 4px;
            }
            QPushButton:hover {
                background-color: #45a049;
            }
            QPushButton:pressed {
                background-color: #3d8b40;
            }
            QPushButton:disabled {
                background-color: #cccccc;
                color: #666666;
            }
        """)

    def create_control_panel(self):
        """創建控制面板"""
        panel = QWidget()
        layout = QVBoxLayout()
        panel.setLayout(layout)

        # 計數器顯示組
        counter_group = QGroupBox("計數器狀態")
        counter_layout = QGridLayout()
        counter_group.setLayout(counter_layout)

        # 計數器值顯示
        counter_layout.addWidget(QLabel("當前計數值:"), 0, 0)
        self.count_display = QLabel("0")
        self.count_display.setStyleSheet("QLabel { font-size: 24px; font-weight: bold; color: #2196F3; }")
        counter_layout.addWidget(self.count_display, 0, 1)

        # 進度條
        counter_layout.addWidget(QLabel("進度:"), 1, 0)
        self.progress_bar = QProgressBar()
        self.progress_bar.setMaximum(1000)  # Counter MAX_COUNT
        counter_layout.addWidget(self.progress_bar, 1, 1)

        # 溢出狀態
        counter_layout.addWidget(QLabel("溢出狀態:"), 2, 0)
        self.overflow_display = QLabel("正常")
        self.overflow_display.setStyleSheet("QLabel { color: green; font-weight: bold; }")
        counter_layout.addWidget(self.overflow_display, 2, 1)

        # 時間顯示
        counter_layout.addWidget(QLabel("模擬時間:"), 3, 0)
        self.time_display = QLabel("0.0 ns")
        counter_layout.addWidget(self.time_display, 3, 1)

        layout.addWidget(counter_group)

        # 基本控制組
        control_group = QGroupBox("基本控制")
        control_layout = QGridLayout()
        control_group.setLayout(control_layout)

        # 控制按鈕
        self.reset_btn = QPushButton("重置計數器")
        self.reset_btn.clicked.connect(self.reset_counter)
        control_layout.addWidget(self.reset_btn, 0, 0)

        self.clear_btn = QPushButton("清零計數器")
        self.clear_btn.clicked.connect(self.clear_counter)
        control_layout.addWidget(self.clear_btn, 0, 1)

        self.increase_btn = QPushButton("增加 +1")
        self.increase_btn.clicked.connect(self.increase_counter)
        control_layout.addWidget(self.increase_btn, 1, 0)

        self.step_btn = QPushButton("單步執行")
        self.step_btn.clicked.connect(self.step_clock)
        control_layout.addWidget(self.step_btn, 1, 1)

        layout.addWidget(control_group)

        # 設置值組
        set_group = QGroupBox("設置計數值")
        set_layout = QGridLayout()
        set_group.setLayout(set_layout)

        set_layout.addWidget(QLabel("設置值:"), 0, 0)
        self.set_value_input = QSpinBox()
        self.set_value_input.setRange(0, 1000)
        self.set_value_input.setValue(0)
        set_layout.addWidget(self.set_value_input, 0, 1)

        self.set_btn = QPushButton("設置計數值")
        self.set_btn.clicked.connect(self.set_counter_value)
        set_layout.addWidget(self.set_btn, 1, 0, 1, 2)

        layout.addWidget(set_group)

        # 模擬控制組
        sim_group = QGroupBox("模擬控制")
        sim_layout = QGridLayout()
        sim_group.setLayout(sim_layout)

        # 自動模式
        self.auto_mode_cb = QCheckBox("自動模式")
        self.auto_mode_cb.stateChanged.connect(self.toggle_auto_mode)
        sim_layout.addWidget(self.auto_mode_cb, 0, 0)

        # 速度控制
        sim_layout.addWidget(QLabel("速度:"), 1, 0)
        self.speed_slider = QSlider(Qt.Horizontal)
        self.speed_slider.setRange(1, 10)
        self.speed_slider.setValue(5)
        self.speed_slider.valueChanged.connect(self.change_speed)
        sim_layout.addWidget(self.speed_slider, 1, 1)

        self.speed_label = QLabel("中等")
        sim_layout.addWidget(self.speed_label, 1, 2)

        # 運行控制
        self.start_btn = QPushButton("開始模擬")
        self.start_btn.clicked.connect(self.start_simulation)
        sim_layout.addWidget(self.start_btn, 2, 0)

        self.stop_btn = QPushButton("停止模擬")
        self.stop_btn.clicked.connect(self.stop_simulation)
        self.stop_btn.setEnabled(False)
        sim_layout.addWidget(self.stop_btn, 2, 1)

        layout.addWidget(sim_group)

        # 添加彈性空間
        layout.addStretch()

        return panel

    def create_display_panel(self):
        """創建顯示面板"""
        panel = QWidget()
        layout = QVBoxLayout()
        panel.setLayout(layout)

        # 日誌顯示組
        log_group = QGroupBox("操作日誌")
        log_layout = QVBoxLayout()
        log_group.setLayout(log_layout)

        self.log_display = QTextEdit()
        self.log_display.setReadOnly(True)
        self.log_display.setMaximumHeight(300)
        log_layout.addWidget(self.log_display)

        # 清除日誌按鈕
        clear_log_btn = QPushButton("清除日誌")
        clear_log_btn.clicked.connect(self.clear_log)
        log_layout.addWidget(clear_log_btn)

        layout.addWidget(log_group)

        # 狀態歷史組
        history_group = QGroupBox("狀態歷史")
        history_layout = QVBoxLayout()
        history_group.setLayout(history_layout)

        self.history_display = QTextEdit()
        self.history_display.setReadOnly(True)
        history_layout.addWidget(self.history_display)

        # 清除歷史按鈕
        clear_history_btn = QPushButton("清除歷史")
        clear_history_btn.clicked.connect(self.clear_history)
        history_layout.addWidget(clear_history_btn)

        layout.addWidget(history_group)

        return panel

    def init_timer(self):
        """初始化定時器"""
        self.update_timer = QTimer()
        self.update_timer.timeout.connect(self.update_display_timer)

    def log_message(self, message):
        """記錄日誌消息"""
        self.log_display.append(f"[{self.testbench.get_current_time():.1f} ns] {message}")
        self.log_display.verticalScrollBar().setValue(
            self.log_display.verticalScrollBar().maximum()
        )

    def update_display_timer(self):
        """定時器更新顯示"""
        data = {
            'count': self.testbench.get_count_out(),
            'overflow': self.testbench.get_overflow(),
            'time': self.testbench.get_current_time()
        }
        self.update_display(data)

    @pyqtSlot(dict)
    def update_display(self, data=None):
        """更新顯示數據"""
        if data is None:
            data = {
                'count': self.testbench.get_count_out(),
                'overflow': self.testbench.get_overflow(),
                'time': self.testbench.get_current_time()
            }

        # 更新計數值顯示
        self.count_display.setText(str(data['count']))

        # 更新進度條
        self.progress_bar.setValue(data['count'])

        # 更新溢出狀態
        if data['overflow']:
            self.overflow_display.setText("溢出")
            self.overflow_display.setStyleSheet("QLabel { color: red; font-weight: bold; }")
        else:
            self.overflow_display.setText("正常")
            self.overflow_display.setStyleSheet("QLabel { color: green; font-weight: bold; }")

        # 更新時間顯示
        self.time_display.setText(f"{data['time']:.1f} ns")

        # 添加到歷史記錄
        history_text = f"時間: {data['time']:.1f}ns | 計數: {data['count']} | 溢出: {'是' if data['overflow'] else '否'}"
        self.history_display.append(history_text)
        self.history_display.verticalScrollBar().setValue(
            self.history_display.verticalScrollBar().maximum()
        )

    # 控制方法
    def reset_counter(self):
        """重置計數器"""
        self.testbench.reset_counter()
        self.log_message("重置計數器")
        self.update_display()

    def clear_counter(self):
        """清零計數器"""
        self.testbench.clear_counter()
        self.log_message("清零計數器")
        self.update_display()

    def increase_counter(self):
        """增加計數器"""
        self.testbench.increase_counter()
        self.log_message("增加計數器 +1")
        self.update_display()

    def step_clock(self):
        """單步時鐘"""
        self.testbench.step_clock(1)
        self.log_message("單步時鐘執行")
        self.update_display()

    def set_counter_value(self):
        """設置計數器值"""
        value = self.set_value_input.value()
        self.testbench.set_counter_value(value)
        self.log_message(f"設置計數器值為 {value}")
        self.update_display()

    def toggle_auto_mode(self, state):
        """切換自動模式"""
        if hasattr(self, 'simulation_worker'):
            self.simulation_worker.auto_mode = (state == Qt.Checked)
            if state == Qt.Checked:
                self.log_message("啟用自動模式")
            else:
                self.log_message("停用自動模式")

    def change_speed(self, value):
        """改變模擬速度"""
        speeds = ["極慢", "很慢", "慢", "較慢", "中慢", "中等", "中快", "較快", "快", "極快"]
        intervals = [1000, 800, 600, 400, 300, 200, 150, 100, 50, 25]

        self.speed_label.setText(speeds[value-1])
        if hasattr(self, 'simulation_worker'):
            self.simulation_worker.step_interval = intervals[value-1]

    def start_simulation(self):
        """開始模擬"""
        if not self.simulation_worker.isRunning():
            self.simulation_worker.start()
            self.start_btn.setEnabled(False)
            self.stop_btn.setEnabled(True)
            self.log_message("開始模擬")

    def stop_simulation(self):
        """停止模擬"""
        if self.simulation_worker.isRunning():
            self.simulation_worker.stop()
            self.start_btn.setEnabled(True)
            self.stop_btn.setEnabled(False)
            self.log_message("停止模擬")

    def clear_log(self):
        """清除日誌"""
        self.log_display.clear()

    def clear_history(self):
        """清除歷史記錄"""
        self.history_display.clear()

    def closeEvent(self, event):
        """窗口關閉事件"""
        if hasattr(self, 'simulation_worker') and self.simulation_worker.isRunning():
            self.simulation_worker.stop()
        event.accept()

def main():
    """主函數"""
    app = QApplication(sys.argv)

    # 設置應用程式屬性
    app.setApplicationName("SystemC Counter Monitor")
    app.setApplicationVersion("1.0")
    app.setOrganizationName("HybridAcc Research")

    # 創建主窗口
    window = CounterMonitorWidget()
    window.show()

    # 初始更新顯示
    window.update_display()
    window.log_message("SystemC Counter 監控器啟動")

    sys.exit(app.exec_())

if __name__ == "__main__":
    main()