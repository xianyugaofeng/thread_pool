# thread_pool

基于 pthread 的 C++ 线程池（header-only：`thread_pool.hpp`）。

## 编译测试

Linux / WSL / MinGW：

```bash
g++ -std=c++11 -Wall -Wextra main.cpp -pthread -o test_pool && ./test_pool
```

建议配合 AddressSanitizer / valgrind 检查内存问题：

```bash
g++ -std=c++11 -g -fsanitize=address,thread main.cpp -pthread -o test_pool && ./test_pool
valgrind --tool=memcheck --leak-check=full ./test_pool
```
