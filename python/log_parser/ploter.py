import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path


class LogPlotter:
	@staticmethod
	def _format_value(value: float) -> str:
		return f"{value:.3g}"

	@staticmethod
	def _annotate_points(ax, x_values: list[str], y_values: list[float]) -> None:
		for x_value, y_value in zip(x_values, y_values):
			ax.annotate(
				LogPlotter._format_value(y_value),
				xy=(x_value, y_value),
				xytext=(0, 6),
				textcoords="offset points",
				ha="center",
				va="bottom",
				fontsize=8,
			)

	@staticmethod
	def plot_metric(
		df: pd.DataFrame,
		metric: str,
		chart_type: str = "bar",
		title: str | None = None,
	):
		if df.empty:
			raise ValueError("No data available to plot.")
		if metric not in df.columns:
			raise ValueError(f"Metric '{metric}' not found in dataframe.")

		plot_df = df[["file", metric]].dropna()
		if plot_df.empty:
			raise ValueError(f"Metric '{metric}' has no valid values.")

		fig, ax = plt.subplots(figsize=(10, 5))
		x = plot_df["file"].tolist()
		y = plot_df[metric].tolist()

		if chart_type == "bar":
			bars = ax.bar(x, y)
			for bar, value in zip(bars, y):
				x_pos = bar.get_x() + bar.get_width() / 2
				y_pos = bar.get_height()
				ax.annotate(
					LogPlotter._format_value(value),
					xy=(x_pos, y_pos),
					xytext=(0, 6),
					textcoords="offset points",
					ha="center",
					va="bottom",
					fontsize=8,
				)
		elif chart_type == "line":
			ax.plot(x, y, marker="o")
			LogPlotter._annotate_points(ax, x, y)
		elif chart_type == "scatter":
			ax.scatter(x, y)
			LogPlotter._annotate_points(ax, x, y)
		else:
			raise ValueError(f"Unsupported chart type: {chart_type}")

		ax.set_xlabel("Log File")
		ax.set_ylabel(metric)
		ax.set_title(title or f"{metric} by log file")
		ax.grid(alpha=0.3)
		ax.tick_params(axis="x", labelrotation=35)
		fig.tight_layout()
		return fig

	@staticmethod
	def save_metric_plot(
		df: pd.DataFrame,
		metric: str,
		chart_type: str,
		output_path: str | Path,
		title: str | None = None,
	) -> Path:
		out = Path(output_path)
		if out.is_dir() or str(output_path).endswith("/"):
			out = out / f"{metric}_{chart_type}.png"
		out.parent.mkdir(parents=True, exist_ok=True)

		fig = LogPlotter.plot_metric(df=df, metric=metric, chart_type=chart_type, title=title)
		fig.savefig(out, dpi=200)
		plt.close(fig)
		return out
