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
    init_cycle_list = []
    single_invoke_cycle_list = []
    last_date = ''

    for i, line in enumerate(all_lines):
      if 'InitializeKeywordRunner' in line and (i + 1) < len(all_lines):
        init_cycle_list.append(int(line.split()[2]))
        single_invoke_cycle_list.append(int(all_lines[i + 1].split()[2]))
        last_date = all_lines[i - 2].strip()

    return init_cycle_list, single_invoke_cycle_list, last_date


def plot_latency_history(init_cycle_list, single_invoke_cycle_list, last_date):
  fig, axs = plt.subplots(2, 2)
  fig.suptitle(
      'Keyword benchmark latency (last update: %s)\nInit: %d ticks Invoke: %d ticks'
      % (last_date, init_cycle_list[-1], single_invoke_cycle_list[-1]))
  fig.set_size_inches(8, 6)
  axs[0, 0].set_title('Text')

  axs[0, 0].plot(init_cycle_list, 'o-')
  axs[0, 0].set_ylabel('Initialize latency (ticks)')

  axs[1, 0].plot(np.concatenate(([0], np.diff(init_cycle_list))), 'o-')
  axs[1, 0].set_ylabel('Incremental change (ticks)')

  axs[0, 1].set_title('Invoke latency')
  axs[0, 1].plot(single_invoke_cycle_list, 'o-')
  axs[1, 1].plot(np.concatenate(([0], np.diff(single_invoke_cycle_list))),
                 'o-')

  plt.subplots_adjust(left=0.12,
                      bottom=0.05,
                      right=0.98,
                      top=0.88,
                      wspace=0.22,
                      hspace=0.1)


if __name__ == '__main__':

  parser = argparse.ArgumentParser()
  parser.add_argument('input_log',
                      help='Path to the size log file (e.g. ~/size_log')
  parser.add_argument(
      '--output_plot',
      help='Path to optionally save plot to (e.g. /tmp/size.png)')
  parser.add_argument('--hide',
                      action='store_true',
                      help='Do NOT show the plot in a matplotlib window.')

  args = parser.parse_args()

  plot_latency_history(*parse_log(args.input_log))

  if args.output_plot:
    plt.savefig(args.output_plot)

  if not args.hide:
    plt.show()
