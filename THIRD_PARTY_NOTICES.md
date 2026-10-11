# Third-Party Notices

本项目 **AllPlay** 是对 **DiPlay** 无线 CarPlay 接收端的 **C++ 重新实现**，
以 **GPL-3.0** 发布，并保留上游许可与署名。

## DiPlay — GPL-3.0

- 仓库：https://github.com/shihabal3amri/DiPlay
- 作者：shihabal3amri
- 说明：Android 无线/有线 CarPlay 接收端（public preview）。本项目的协议行为、流程与
  wire 格式参照其实现与整理。

## xcertplay — GPL-3.0

- 说明：DiPlay 的上游项目（作者 shilapi），CarPlay 接收端协议实现，GPL-3.0。

## 协议研究参考

- **LIVI**、**Showcase**：AirPlay / iAP2 / CarPlay 协议研究（DiPlay 亦在其署名中致谢）。

## MFi 配件身份

- `identity/` 下的 MFi 配件身份（`identity.pk8` + `certificate.p7b`）与 DiPlay 使用同一
  来源：公开的 Carlinkit C2Air 固件中恢复出的实验性配件身份。
- 该凭证**不入库**（见 `.gitignore`），需自行准备。
