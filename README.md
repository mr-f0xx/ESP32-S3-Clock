# ESP32-S3 AMOLED LVGL Clock

适用于 **ESP32-S3-Touch-AMOLED-2.16** 的 LVGL 桌面时钟固件，集成相册、动态天气壁纸、天气预报、视频与音乐播放、闹钟、日历、自动旋转和网页设置。

> ## 快速下载
>
> **[下载 16 MB 固件](https://github.com/maxiumlee/ESP32-S3-LVGL-Clock/releases/latest/download/LVGL_Clock_16MB.merged.bin)** 　|　 **[浏览原始 JWP 天气壁纸](https://github.com/maxiumlee/ESP32-S3-LVGL-Clock/tree/main/tfcard/wallpaper)** 　|　 **[下载乐鑫官方烧录工具](https://www.espressif.com/en/tools-type/flash-download-tools)**
>
> 烧录只需要固件和官方工具；JWP 天气壁纸为可选下载，不需要安装 Arduino IDE、Python 或第三方烧录器。
>
> 天气视频素材网盘链接：https://drive.google.com/drive/folders/1uimT8YDarJMT8KugjhhC21ljjzA2iPPn?usp=sharing

## 1. 烧录固件

1. 下载并解压乐鑫官方 **Flash Download Tool**。
2. 启动工具，选择：

   | 选项 | 设置 |
   | --- | --- |
   | ChipType | `ESP32-S3` |
   | WorkMode | `Develop` |
   | LoadMode | `UART` |

3. 在烧录列表第一行选择 `LVGL_Clock_16MB.merged.bin`，勾选该行，地址填写 `0x0`。
4. 设置烧录参数（若可选）：

   | 参数 | 设置 |
   | --- | --- |
   | SPI SPEED | `40MHz` |
   | SPI MODE | `DIO` |
   | FLASH SIZE | `16MB` |
   | BAUD | `460800`（推荐） |

5. 选择设备对应的 COM 口，点击 `START`。显示 `FINISH` 后再断开 USB。

如果工具一直等待连接：按住开发板 `BOOT`的同时，长按 `PWR`，等待设备重启后松手。

> 这是完整 16 MB 镜像，会更新程序、分区及设备设置。重新烧录后需要再次配置 Wi-Fi 和天气。

固件 SHA-256：

```text
6557564C6859FBE129E36B43A6F6F7C870472E6F61734282894BD8819C090B3C
```

Windows 校验命令：

```powershell
Get-FileHash .\LVGL_Clock_16MB.merged.bin -Algorithm SHA256
```

## 2. 首次启动与配置

1. 插入 FAT32 格式的 TF 卡并启动设备。
2. 默认进入 App 选择界面第一页，打开 灰色图标的**设置**。
3. 手机或电脑连接设备创建的开放热点 `AMOLED-Clock-XXXXXX`，无需密码。
4. 浏览器打开 `http://192.168.4.1`。
5. 填写家庭 Wi-Fi 并保存。
6. 在天气设置中填写和风天气控制台提供的 **API Host** 和 **API Key**。
7. 定位方式可选择自动定位，也可以手动填写经纬度。
8. 退出 Settings 后，设备热点会自动关闭。

公开固件不包含个人天气 API Key。请勿把自己的 Key 提交到公开仓库。

## 3. 准备 TF 卡

在 TF 卡根目录创建以下文件夹：

```text
/wallpaper/                 动态天气壁纸
/music/                     MP3/WAV 音乐
/video/                     MJPEG 视频及同名 WAV 音频
/video/rotated/             竖屏预旋转视频
```

转换完成的 JWP 已按天气类型和屏幕方向保存在 [`tfcard/wallpaper`](tfcard/wallpaper)，并由 Git LFS 管理。需要完整下载源码和壁纸时先安装 Git LFS，再执行：

```powershell
git lfs install
git clone https://github.com/maxiumlee/ESP32-S3-LVGL-Clock.git
```

视频文件示例：

```text
/video/example.mjpeg
/video/example.wav
/video/rotated/example.mjpeg
```

普通视频可使用 [Windows 视频转换工具](tools/windows_converter/AMOLED_MJPEG_Converter_FFmpeg_Windows_x64.zip) 转换。选择“直接生成到 TF 卡”后，工具会自动创建缺失目录并把文件写入正确位置。

## 4. 主要功能

- App 菜单、相册时钟与自动图片切换
- 和风天气当前信息、三日预报、AQI、小时温度曲线和分钟降水曲线
- 根据天气自动切换动态壁纸
- MJPEG + WAV 视频播放、进度拖动和断点记忆
- MP3/WAV 音乐播放
- 闹钟、日历、自动屏幕旋转
- 无操作休眠、息屏时间显示和低功耗恢复
- 网页配置 Wi-Fi、天气、相册及休眠选项
- 串口 `W/A/S/D` 手势模拟，`W` 表示上划

## 5. 工程目录

```text
arduino/08_LVGL_Clock/     主程序源码
arduino/libraries/         工程使用的固定版本依赖
firmware/                  16 MB 合并固件
scripts/                   固件构建脚本
tools/weather_ui_preview/  天气界面网页预览与坐标调整工具
tools/wallpaper_builder/   MP4 转动态天气壁纸脚本
tools/windows_converter/   普通视频转 MJPEG/WAV 工具
tfcard/                    TF 卡目录结构
licenses/                  字体与图标许可说明
```

本地源码配置文件 `clock_weather_config.h` 和 `clock_weather_config.private.h` 已被 `.gitignore` 排除。公开模板位于：

```text
arduino/08_LVGL_Clock/clock_weather_config.example.h
```

## 6. 重新生成天气壁纸

需要 Python 3.10 或更高版本：

```powershell
python -m pip install -r tools/wallpaper_builder/requirements.txt
python tools/wallpaper_builder/build_playlist.py
```

脚本读取 `media/source_mp4/*.mp4`，生成横屏和竖屏资源到 `tfcard/wallpaper`。转换完成的 JWP 通过 Git LFS 发布；大型源 MP4、音乐和普通视频不提交到仓库，可通过网盘或其他存储单独发布。

## 许可

项目许可见 [LICENSE](LICENSE)。字体与 Tabler 图标的许可说明位于 [licenses](licenses)。
