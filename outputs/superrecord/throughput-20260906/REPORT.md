# 已完成实验吞吐量快照

生成时间：2026-09-06T06:24:11.677991+00:00

口径：档位逻辑容量 / 平均服务器耗时，MiB/s；1 GiB=1024 MiB。每次服务完整路径；不含离线预处理、密钥生成、客户端解码和网络传输时间。吞吐量按容量除以平均耗时计算。

仅纳入已生成结果、进程退出码为0、样本数与正确性验证通过的正式实验。空格表示尚无完成结果。

| 方案 | Wisconsin 1 GiB | Wisconsin 4 GiB | Clemson 1 GiB | Clemson 8 GiB |
|---|---:|---:|---:|---:|
| Generic best h3 | 1083.87 (100次) | 1271.07 (100次) | 509.39 (100次) | — |
| Generic fastest h1 | 909.87 (100次) | 1156.29 (100次) | 449.03 (100次) | — |
| Layerwise tuned | 748.10 (100次) | 1106.20 (100次) | 390.38 (100次) | 664.32 (100次) |
| Tree g=32 | 787.71 (100次) | 862.14 (100次) | 386.85 (100次) | 431.32 (100次) |
| Cuckoo tuned | 201.43 (16次) | 321.71 (16次) | 106.34 (100次) | 205.50 (100次) |
| TreePIR | 115.54 (16次) | — | 29.55 (100次) | — |
| Flat | 51.85 (16次) | 54.15 (16次) | 27.51 (16次) | — |

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
| Wisconsin 1gb | TreePIR | 8.862836 | 8.872762 | 1179987 | 131185 |
| Wisconsin 1gb | Flat | 19.750169 | 20.093897 | 357120 | 270336 |
| Wisconsin 4gb | Generic best h3 | 3.222477 | 3.225089 | 14880 | 11264 |
| Wisconsin 4gb | Generic fastest h1 | 3.542363 | 3.545947 | 14880 | 11264 |
| Wisconsin 4gb | Layerwise tuned | 3.702769 | 3.740814 | 297600 | 229312 |
| Wisconsin 4gb | Tree g=32 | 4.750985 | 4.856292 | 14880 | 11264 |
| Wisconsin 4gb | Cuckoo tuned | 12.731994 | 12.738365 | 595200 | 450560 |
| Wisconsin 4gb | Flat | 75.647486 | 77.182476 | 386880 | 292864 |

查询除TreePIR为实际unseeded序列化外均为模型估算；响应为实际序列化。TreePIR的Clemson1GiB为旧VBPIR默认首维128；Wisconsin1GiB已完成VBPIR首维256的独立16次正式复测。4GiB后端正式复测仍在运行。

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
| Wisconsin 1gb | 0.825110 | 1241.05 | 3072 | 100 |
| Wisconsin 4gb | 2.919728 | 1402.87 | 3072 | 100 |

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

## 当前执行状态

Wisconsin1GiB全部完成，4GiB除TreePIR正式复测外全部完成，包含后加的Standard单块参考。TreePIR1GiB正式16次选中vbpir_fd256，8862.8358ms、115.5387MiB/s。4GiB正在正式复测SealPIR二维；其16次pilot为101162.7211ms、40.4892MiB/s，该pilot未进入正式主表。

Clemson8GiB Generic正式运行失败且无完整计时样本。六布局×三查询的诊断已经完成：18条全部解密正确，17条满足1-bit余量，唯一短缺出现在h1/G8/P3/N0=512。三个h3布局的9条诊断均满足阈值，但不替代100次正式测量。原父队列仍暂停，后续TreePIR、Flat和entry-size队列未恢复。

## TreePIR本轮候选限制

| 档位/候选 | 结果 |
|---|---|
| 1GiB VBPIR fd64 | step count too large，退出1 |
| 4GiB VBPIR fd64 | OOM，退出-9；内核日志确认 |
| 4GiB VBPIR fd128 | OOM，退出-9；内核日志确认 |
| 4GiB VBPIR fd256 | noise budget is zero，退出1 |
| 4GiB SealPIR d1 | nvec超过poly degree，退出1 |
| 4GiB SealPIR d2 | pilot16/16正确；独立正式16次进行中 |

本轮TreePIR比较范围为可运行的VBPIR与SealPIR参数网格。不同档位/服务器可能选用不同后端与HE参数。它不是所有TreePIR基底模型的全局最优结论。
