import argparse
import matplotlib.pyplot as plt
import numpy as np


def parse_log(log_name):
  """ Parses the raw log into int lists of sizes for text, data, bss

    Args:
      log_name: full path to the log file.

    Returns:
      text_size_list, data_size_list, bss_size_list
  """
  with open(log_name) as file:
    all_lines = file.readlines()
    text_size_list = []
    data_size_list = []
    bss_size_list = []
    last_date = ''

    for i, line in enumerate(all_lines):
      if 'text' in line and (i + 1) < len(all_lines):
        text_size, data_size, bss_size = all_lines[i + 1].split()[0:3]
        text_size_list.append(int(text_size))
        data_size_list.append(int(data_size))
        bss_size_list.append(int(bss_size))
        last_date = all_lines[i - 2].strip()

    return text_size_list, data_size_list, bss_size_list, last_date


def plot_size_history(text_size_list, data_size_list, bss_size_list, last_date):
  fig, axs = plt.subplots(2, 3)
  fig.suptitle(
      'Size evolution for keyword_benchmark (last update: %s)'
      % (last_date))
  fig.set_size_inches(12, 6)
  axs[0, 0].set_title('Text')

  axs[0, 0].plot(text_size_list, 'o-')
  axs[0, 0].set_ylabel('Absolute size (bytes)')

  axs[1, 0].plot(np.concatenate(([0], np.diff(text_size_list))), 'o-')
  axs[1, 0].set_ylabel('Incremental change (bytes)')

  axs[0, 1].set_title('Data')
  axs[0, 1].plot(data_size_list, 'o-')
  axs[1, 1].plot(np.concatenate(([0], np.diff(data_size_list))), 'o-')

  axs[0, 2].set_title('BSS')
  axs[0, 2].plot(bss_size_list, 'o-')
  axs[1, 2].plot(np.concatenate(([0], np.diff(bss_size_list))), 'o-')
  plt.subplots_adjust(
      left=0.08, bottom=0.05, right=0.98, top=0.90, wspace=0.22, hspace=0.1)


if __name__ == '__main__':

  parser = argparse.ArgumentParser()
  parser.add_argument(
      'input_log', help='Path to the size log file (e.g. ~/size_log')
  parser.add_argument(
      '--output_plot',
      help='Path to optionally save plot to (e.g. /tmp/size.png)')
  parser.add_argument(
      '--hide',
      action='store_true',
      help='Do NOT show the plot in a matplotlib window.')

  args = parser.parse_args()

  plot_size_history(*parse_log(args.input_log))

  if args.output_plot:
    plt.savefig(args.output_plot)

  if not args.hide:
    plt.show()
