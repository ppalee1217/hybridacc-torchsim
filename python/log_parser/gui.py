from __future__ import annotations

from pathlib import Path
from typing import List

import pandas as pd
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Button, Checkbox, DataTable, Footer, Header, Input, Label, Select, Static

from .parser import NocSimLogParser
from .ploter import LogPlotter


PARSE_TYPE_OPTIONS = [("noc_sim", "noc_sim")]
CHART_OPTIONS = [("Bar", "bar"), ("Line", "line"), ("Scatter", "scatter")]


class LogParserApp(App):
	TITLE = "HybridAcc Log Parser"

	CSS = """
	#main-layout { height: 1fr; }
	#left-panel, #right-panel { width: 1fr; padding: 1; }
	#log-list { height: 14; border: solid gray; }
	#controls-row { height: auto; }
	#actions-row { height: auto; }
	#result-table { height: 1fr; }
	.section-title { text-style: bold; }
	"""

	def __init__(self, parse_type: str = "noc_sim"):
		super().__init__()
		self.parser = NocSimLogParser(parse_type=parse_type)
		self.parsed_df = pd.DataFrame()
		self.log_entries: List[dict] = []
		self.selected_log_index: int | None = None

	def compose(self) -> ComposeResult:
		yield Header()
		with Horizontal(id="main-layout"):
			with Vertical(id="left-panel"):
				yield Label("1) 解析類型與資料夾", classes="section-title")
				yield Select(options=PARSE_TYPE_OPTIONS, value="noc_sim", id="parse-type")
				yield Input(placeholder="log 資料夾路徑，例如 ./output", id="folder-input")
				with Horizontal(id="controls-row"):
					yield Button("掃描 Log", id="scan-btn", variant="primary")
					yield Button("全選", id="select-all-btn")
					yield Button("全不選", id="unselect-all-btn")
					yield Button("上移", id="move-up-btn")
					yield Button("下移", id="move-down-btn")

				yield Label("2) 勾選多個 Log 檔案", classes="section-title")
				with VerticalScroll(id="log-list"):
					yield Static("尚未掃描", id="log-list-empty")

				yield Label("3) 點選分析", classes="section-title")
				yield Button("分析", id="analyze-btn", variant="success")

			with Vertical(id="right-panel"):
				yield Label("4) 選擇統計數據與圖表", classes="section-title")
				yield Select(options=[("請先分析", "")], value="", id="metric-select")
				yield Select(options=CHART_OPTIONS, value="bar", id="chart-select")
				yield Input(placeholder="輸出路徑，例如 ./output/plot.png", id="output-input")
				with Horizontal(id="actions-row"):
					yield Button("儲存圖表", id="save-btn", variant="primary")

				yield Label("分析結果", classes="section-title")
				yield DataTable(id="result-table")
		yield Footer()

	def on_mount(self) -> None:
		table = self.query_one("#result-table", DataTable)
		table.cursor_type = "row"

	def on_button_pressed(self, event: Button.Pressed) -> None:
		button_id = event.button.id
		if button_id == "scan-btn":
			self._scan_logs()
		elif button_id == "select-all-btn":
			self._set_all_checks(True)
		elif button_id == "unselect-all-btn":
			self._set_all_checks(False)
		elif button_id == "move-up-btn":
			self._move_selected(-1)
		elif button_id == "move-down-btn":
			self._move_selected(1)
		elif button_id == "analyze-btn":
			self._analyze_selected_logs()
		elif button_id == "save-btn":
			self._save_plot()

	def on_checkbox_changed(self, event: Checkbox.Changed) -> None:
		checkbox = event.checkbox
		if checkbox.name is None:
			return

		path_text = str(checkbox.name)
		for index, entry in enumerate(self.log_entries):
			if entry["path"] == path_text:
				entry["checked"] = checkbox.value
				self.selected_log_index = index
				self._render_log_list()
				return

	def _scan_logs(self) -> None:
		folder = self.query_one("#folder-input", Input).value.strip()
		if not folder:
			self.notify("請先輸入 log 資料夾", severity="warning")
			return

		try:
			files = self.parser.list_log_files(folder)
		except Exception as exc:
			self.notify(str(exc), severity="error")
			return

		if not files:
			container = self.query_one("#log-list", VerticalScroll)
			self._clear_log_list(container)
			container.mount(Static("找不到 .log 檔案", id="log-list-empty"))
			return

		self.log_entries = [{"path": str(file_path), "checked": True} for file_path in files]
		self.selected_log_index = 0 if self.log_entries else None
		self._render_log_list()

		self.notify(f"已載入 {len(files)} 個 log 檔案")

	def _clear_log_list(self, container: VerticalScroll) -> None:
		for child in list(container.children):
			child.remove()

	def _set_all_checks(self, checked: bool) -> None:
		for entry in self.log_entries:
			entry["checked"] = checked
		self._render_log_list()

	def _selected_file_paths(self) -> List[Path]:
		return [Path(entry["path"]) for entry in self.log_entries if entry["checked"]]

	def _move_selected(self, direction: int) -> None:
		if self.selected_log_index is None or not self.log_entries:
			self.notify("請先點選一個 log 檔案", severity="warning")
			return

		target_index = self.selected_log_index + direction
		if target_index < 0 or target_index >= len(self.log_entries):
			return

		self.log_entries[self.selected_log_index], self.log_entries[target_index] = (
			self.log_entries[target_index],
			self.log_entries[self.selected_log_index],
		)
		self.selected_log_index = target_index
		self._render_log_list()

	def _render_log_list(self) -> None:
		container = self.query_one("#log-list", VerticalScroll)
		self._clear_log_list(container)

		if not self.log_entries:
			container.mount(Static("尚未掃描", id="log-list-empty"))
			return

		for index, entry in enumerate(self.log_entries):
			file_name = Path(entry["path"]).name
			prefix = "▶ " if self.selected_log_index == index else "  "
			container.mount(
				Checkbox(
					f"{prefix}{file_name}",
					value=bool(entry["checked"]),
					name=str(entry["path"]),
				)
			)

	def _analyze_selected_logs(self) -> None:
		parse_type = self.query_one("#parse-type", Select).value
		if parse_type != "noc_sim":
			self.notify("目前僅支援 noc_sim", severity="warning")
			return

		selected_files = self._selected_file_paths()
		if not selected_files:
			self.notify("請至少勾選一個 log 檔案", severity="warning")
			return

		try:
			self.parsed_df = self.parser.parse_files(selected_files)
		except Exception as exc:
			self.notify(str(exc), severity="error")
			return

		self._render_table(self.parsed_df)
		self._refresh_metric_options(self.parsed_df)
		self.notify(f"分析完成：{len(self.parsed_df)} 個檔案")

	def _render_table(self, df: pd.DataFrame) -> None:
		table = self.query_one("#result-table", DataTable)
		table.clear(columns=True)
		if df.empty:
			return

		table.add_columns(*[str(col) for col in df.columns])
		for _, row in df.iterrows():
			values = [self._format_cell_value(row[col]) for col in df.columns]
			table.add_row(*values)

	def _refresh_metric_options(self, df: pd.DataFrame) -> None:
		metric_select = self.query_one("#metric-select", Select)
		numeric_cols = self.parser.numeric_columns(df)
		if not numeric_cols:
			metric_select.set_options([("無可用數值欄位", "")])
			metric_select.value = ""
			return

		options = [(col, col) for col in numeric_cols]
		metric_select.set_options(options)
		metric_select.value = numeric_cols[0]

	def _save_plot(self) -> None:
		if self.parsed_df.empty:
			self.notify("請先分析 log 檔案", severity="warning")
			return

		metric = self.query_one("#metric-select", Select).value
		chart = self.query_one("#chart-select", Select).value
		output_text = self.query_one("#output-input", Input).value.strip()

		if not metric:
			self.notify("請先選擇統計數據", severity="warning")
			return
		if not chart:
			self.notify("請先選擇圖表類型", severity="warning")
			return
		if not output_text:
			self.notify("請先輸入輸出路徑", severity="warning")
			return

		try:
			saved_path = LogPlotter.save_metric_plot(
				df=self.parsed_df,
				metric=str(metric),
				chart_type=str(chart),
				output_path=output_text,
			)
		except Exception as exc:
			self.notify(str(exc), severity="error")
			return

		self.notify(f"圖表已儲存：{saved_path}", severity="information")

	@staticmethod
	def _format_cell_value(value: object) -> str:
		if isinstance(value, float):
			return f"{value:.6g}"
		return str(value)


def run_gui(parse_type: str = "noc_sim") -> None:
	app = LogParserApp(parse_type=parse_type)
	app.run()


if __name__ == "__main__":
	run_gui()
