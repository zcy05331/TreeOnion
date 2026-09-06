# 已完成实验吞吐量快照

生成时间：2026-09-06T00:25:37.560066+00:00

口径：档位逻辑容量 / 平均服务器耗时，MiB/s；1 GiB=1024 MiB。每次服务完整路径；不含离线预处理、密钥生成、客户端解码和网络传输时间。吞吐量按容量除以平均耗时计算。

仅纳入已生成结果、进程退出码为0、样本数与正确性验证通过的正式实验。空格表示尚无完成结果。

| 方案 | Wisconsin 1 GiB | Wisconsin 4 GiB | Clemson 1 GiB | Clemson 8 GiB |
|---|---:|---:|---:|---:|
| Generic best h3 | 1083.87 (100次) | — | 509.39 (100次) | — |
| Generic fastest h1 | 909.87 (100次) | — | 449.03 (100次) | — |
| Layerwise tuned | 748.10 (100次) | — | 390.38 (100次) | 664.32 (100次) |
| Tree g=32 | 787.71 (100次) | — | 386.85 (100次) | 431.32 (100次) |
| Cuckoo tuned | 201.43 (16次) | — | 106.34 (100次) | 205.50 (100次) |
| TreePIR | — | — | 29.55 (100次) | — |
| Flat | 51.85 (16次) | — | 27.51 (16次) | — |

## 耗时与通信

| 服务器/档位 | 方案 | 平均服务器秒 | 95分位秒 | 查询B | 响应B |
|---|---|---:|---:|---:|---:|
| Clemson 1gb | Generic best h3 | 2.010239 | 2.015120 | 14880 | 11264 |
| Clemson 1gb | Generic fastest h1 | 2.280484 | 2.285160 | 14880 | 11264 |
| Clemson 1gb | Layerwise tuned | 2.623115 | 2.629962 | 267840 | 206784 |
| Clemson 1gb | Tree g=32 | 2.647043 | 2.667862 | 14880 | 11264 |
| Clemson 1gb | Cuckoo tuned | 9.629532 | 9.647065 | 550560 | 416768 |
| Clemson 1gb | TreePIR | 34.653341 | 34.749940 | 1179987 | 131185 |
| Clemson 1gb | Flat | 37.225532 | 37.449026 | 357120 | 270336 |
| Clemson 8gb | Layerwise tuned | 12.331324 | 12.378551 | 312480 | 240576 |
| Clemson 8gb | Tree g=32 | 18.992701 | 19.052811 | 14880 | 11264 |
| Clemson 8gb | Cuckoo tuned | 39.864600 | 39.874378 | 624960 | 473088 |
| Wisconsin 1gb | Generic best h3 | 0.944762 | 0.944996 | 14880 | 11264 |
| Wisconsin 1gb | Generic fastest h1 | 1.125430 | 1.126101 | 14880 | 11264 |
| Wisconsin 1gb | Layerwise tuned | 1.368807 | 1.377192 | 267840 | 206784 |
| Wisconsin 1gb | Tree g=32 | 1.299968 | 1.316138 | 14880 | 11264 |
| Wisconsin 1gb | Cuckoo tuned | 5.083728 | 5.084640 | 550560 | 416768 |
| Wisconsin 1gb | Flat | 19.750169 | 20.093897 | 357120 | 270336 |

查询除TreePIR为实际unseeded序列化外均为模型估算；响应为实际序列化。TreePIR旧数据使用VBPIR默认参数，新后端选参尚未完成。

## 历史对照附表

| 服务器/档位 | 旧对照 | MiB/s | 次数 |
|---|---|---:|---:|
| Clemson 1gb | Layerwise legacy | 320.98 | 100 |
| Clemson 1gb | Cuckoo legacy | 77.68 | 100 |
| Clemson 1gb | Tree compressed | 388.60 | 100 |
| Clemson 8gb | Layerwise legacy | 395.71 | 100 |
| Clemson 8gb | Cuckoo legacy | 51.31 | 100 |
| Clemson 8gb | Tree compressed | 431.25 | 100 |

## 单块OnionPIRv2参考

此处每次返回一个3KiB连续明文块，不完成完整路径，因此单独列示。

| 服务器/档位 | 平均服务器秒 | MiB/s | 返回块B | 次数 |
|---|---:|---:|---:|---:|
| Clemson 1gb | 1.544023 | 663.20 | 3072 | 100 |
| Clemson 8gb | 10.518811 | 778.80 | 3072 | 100 |

## Cuckoo选参附表（pilot）

| 服务器/档位 | 展开高度 | N0 | MiB/s | 次数 |
|---|---:|---:|---:|---:|
| Clemson 1gb | 10 | 512 | 84.07 | 16 |
| Clemson 1gb | 7 | 64 | 77.85 | 16 |
| Clemson 1gb | 8 | 128 | 102.49 | 16 |
| Clemson 1gb | 9 | 256 | 104.58 | 16 |
| Clemson 8gb | 10 | 512 | 198.75 | 16 |
| Clemson 8gb | 7 | 32 | 51.36 | 16 |
| Clemson 8gb | 8 | 128 | 140.39 | 16 |
| Clemson 8gb | 9 | 256 | 185.12 | 16 |
| Wisconsin 1gb | 10 | 512 | 171.67 | 16 |
| Wisconsin 1gb | 7 | 64 | 136.82 | 16 |
| Wisconsin 1gb | 8 | 128 | 189.49 | 16 |
| Wisconsin 1gb | 9 | 256 | 201.99 | 16 |

## 范围

Generic/g32返回含根路径，Layerwise/Cuckoo/TreePIR/Flat为非根层路径。保留既有语义。Standard单次查找和g1的12bit标量数据不进入32B路径吞吐量对照。旧node0历史运行也未并入当前两台服务器的表格。各方案在独立进程执行，跨方案倍数为描述性比较。

Generic完整候选及形状见generic-candidates.csv。阶段均值见stage-times.csv；原始路径、样本数、标准差、通信口径见formal-throughput.csv。选参pilot与正式试验分开，主表不采用pilot。

## 本轮主要发现

Wisconsin 1 GiB 最佳 h3 为944.762 ms、1083.871 MiB/s，形状h3/G64/P1/N0=352/b9；最快h1为1125.430 ms、909.874 MiB/s，形状h1/G8/P3/N0=512/b7。两者均100次，查询模型14880B、响应实际11264B。h3较g32耗时降低27.32%、吞吐提高37.60%；较Layerwise tuned耗时降低30.98%。

Clemson同机1GiB的Standard单块为1544.023 ms，h3完整含根路径为2010.239 ms。h3服务器耗时是一次Standard的1.302倍；在线查询/响应相同，一次性辅助密钥模型1488000B与1607040B（h3多8%）。新Wisconsin服务器没有新增Standard单块测量。
