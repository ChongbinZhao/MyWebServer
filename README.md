#### **原项目地址**

[qinguoyi/TinyWebServer: Linux下C++轻量级Web服务器 (github.com)](https://github.com/qinguoyi/TinyWebServer)

<br>

#### **改进之处**

- **前端界面**
  - 原项目后端的实现逻辑非常严谨，但是前端网页排版比较混乱。
  - 我对前端`HTML`页面进行了细微的调整，让页面更简洁工整。
- **代码注释**
  - 原项目代码量比较大，源代码中注释比较少，很多变量和函数虽然名字类似但实际含义不同。
  - 我根据自己的理解在代码中加入了大量的注释。
- **思维导图**
  - 原项目将系统的功能模块化，给出了系统实现的[概念图]([687474703a2f2f7777312e73696e61696d672e636e2f6c617267652f303035544a3263376c79316765306a3161747135686a33306736306c6d3077342e6a7067 (582×778) (camo.githubusercontent.com)](https://camo.githubusercontent.com/8813be3bb9590eba2207d27b95404ec996891960b47ebb3a447b6c943c0b714d/687474703a2f2f7777312e73696e61696d672e636e2f6c617267652f303035544a3263376c79316765306a3161747135686a33306736306c6d3077342e6a7067))，但项目中函数的功能以及函数之间的调用关系还是比较难捋清。
  - 于是我从`main()`函数出发，将核心的功能函数通过思维导图的方式来展开，从更细致的角度来捋清项目代码的实现过程。

![](https://raw.githubusercontent.com/ChongbinZhao/MyWebServer/cea76e011c2625c5c2b25b7542e2f1e65b0e35cc/root/mindTree.svg)