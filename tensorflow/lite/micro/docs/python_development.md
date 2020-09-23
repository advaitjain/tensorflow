
# Contributing Python Code

For TensorFlow Lite Micro, we sometimes manipulate the TfLite flatbuffer models
with Python scripts to make the models more suitable to run on embedded targets.
Much of the common functionality is in the tensorflow/lite/tools folder (see the
py_binary and py_library targets in the BUILD file).


Tensorflow uses bazel to manage 

The first step to contributing Python code for TFLM is to build the 


# Style Guide and Formatting

*   [TensorFlow guide](https://www.tensorflow.org/community/contribute/code_style#python_style)
    for Python development

*   [yapf](https://github.com/google/yapf/) should be used for formatting.

    ```
    yapf log_parser.py -i --style='{based_on_style: pep8, indent_width: 2}'
    ```


# General Tips

Since building a Python target can involve rebuilding large parts of the overall
Tensorflow codebase, it might be helpful to create a separate clone of the
tensorflow repo for Python development to avoid inadvertenly invalidating the
bazel cache and causing a rebuild.


