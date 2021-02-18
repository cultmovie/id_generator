# 基于skynet的id生成器

# 一.设计目标
1. 满足一定条件后，生成单进程内不重复的整数id，字符串UUID不在考虑范围内，多进程唯一id不在考虑范围内
2. lua最大整数 64bit，因此id不可超出64bit
3. 保证lua哈希表的效率，减少哈希冲突
4. id要递增，方便排序
5. 宕机重启后，新生成的id跟老的id不重复，即id不会回档
6. 不依赖三方软件，不用call
7. 发生系统时钟回拨时，生成的id不能重复

# 二.设计方案

## 1.数据格式
```
0 00000000 00 00000000 0000 00000000 00000000 00000000 00000000 00000000 0
| |reserved | |serial num | |---------------timestamp--------------------|
```

  最高位为符号位，不使用
  timestamp够用(2^41)/(3600 \* 24 \* 365 \* 1000)=69.7年,单位毫秒
  serial num:每毫秒最多可生成2^12=4096个
  
## 2.实现
支持两种方式：
1.id分配放到c层，所有服务共享，需要线程同步，可以保证进程内生成的id不重复
2.id分配放到单个服务，不需要线程同步，只能保证单服务内生成的id不重复

需要支持多个id种类，属于同一种类的id必须不重复，不同种类的id可以重复
## 3.时钟回拨
有两种情况要考虑时钟回拨：进程运行中和进程重启
有几种引起时钟回拨的可能
* 夏令时结束时，系统时钟会回拨（我国于1992年取消夏令时，国服可以不考虑，但海外服是必须要考虑的）
* ubuntu使用systemd-timesyncd.service服务从ntp服务器自动同步时钟，ntp服务器有可能发生ms级别的时钟波动
* 人工回拨系统时钟
 
 在进程运行中，可以使用CLOCK_MONOTONIC参数调用clock_gettime接口获得单调递增时间，可以保证运行中的进程获得的时间不受系统时钟调整的影响
 
但在进程关闭后，重启前，由于某种原因回拨了系统时钟（例如回拨了一个小时），则会发生id重复，因为skynet启动后会用CLOCK_REALTIME参数调用clock_gettime接口获得时间，来重置进程启动时间；从ntp服务器同步过来的时钟可能会有ms级别的波动，但大范围波动概率极低，可以不予考虑；至于人工回拨时钟，可以让系统管理员来保证不会发生；夏令时结束引起的时钟回拨，可以通过停服，度过1个小时的回拨时间，再启动服务器，需要一年做一次，好在国服不用考虑夏令时

## 4.减少lua哈希表的冲突
lua table的哈希部分采用的是开放地址法，哈希冲突过多，会引起性能下降
要保证哈希表的效率，必须保证生成的绝大部分id的低位尽量不同，因为lua table整数的hash函数如下：

```
#define lmod(s,size) \                                              
      (check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))
```

(s) & ((size)-1)表明，在哈希表的长度size一定的情况下，s的低位差异大，则哈希表冲突的概率就低
因此我们选择把timestamp放在最低的41位，因为timestamp单位是ms，且单调递增，能够保证绝大部分情况下低位不同
但假如在循环中分配大量id，导致1ms内生成的id低位都相同，引起哈希表冲突增多，如果使用场景很在意哈希表的效率，可以在循环中使用skynet.hsleep(1)来阻塞1ms即可

# 三.性能
支持多种id种类，各个模块间id可以重复
id种类为100，16个服务，8核，cpu：Intel i7-8750H 2.20GHz
每个服务循环生成10w个id,种类在1-100间随机，自旋锁
每个服务耗时27-29ms

哈希表性能：timestamp单调递增的情况下，性能跟从1开始自增的id性能相当

# 四.下一步
1.id缓存
2.多进程唯一id

# 五.其他
在调试这个模块时，有一个意外的发现：
编译完该模块后，生成了id_generator.so
并在main服务中执行了如下代码：

```
local Skynet = require "skynet"
local IdGenerator = require "id_generator"
local 
IdGenerator.init()
...
Skynet.exit()
```
除了main服务之外，没有其他服务再require "id_generator"

服务器启动后，使用热更新机制，在任一服务中调用id_generator这个c模块中的全局static变量时，会发现该变量被重新初始化了
查证过程不赘述，结论如下：

在我们的案例中，skynet服务调用Skynet.exit()销毁自己，而lua虚拟机销毁时会调用dlclose，lua在调用dlopen时并未使用选项RTLD_NODELETE，dlclose会将id_generator.so在连接器中的引用计数减为0，从而导致id_generator.so从内存中卸载，使用热更新机制在某个服务中重新require "id_generator"时，就会重新初始化该so文件中的全局static变量
