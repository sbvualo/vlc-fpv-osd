Lang: [Русский](README.md) [English](README_en.md)

# VLC-FPV-OSD
Плагин для медиапроигрывателя VLC

Накладывает OSD при воспроизведении видеозаписи с FPV очков DJI Goggles

## Сборка
### Windows

Установить [MSYS2](https://www.msys2.org)

Скачать с сайта [videolan.org](https://download.videolan.org/pub/videolan/vlc/) архив в формате 7z и распаковать. Далее `<VLC_DIR>` - распакованная папка.

Исправить содержимое файла `<VLC_DIR>\sdk\lib\pkgconfig\vlc-plugin.pc`:

```
prefix=<VLC_DIR>/sdk
pluginsdir=<VLC_DIR>/plugins
```

Запустить MSYS2 MinGW64 и выполнить команды:

```bash
export PKG_CONFIG_PATH=<VLC_DIR>/sdk/lib/pkgconfig:$PKG_CONFIG_PATH
make
make install
```

## Установка
Скопировать выходной файл `libfpvosd_plugin.dll` (для Windows) в папку с плагинами VLC `plugins/misc`.

## Как пользоваться
Настройки находятся в режиме расширенных настроек в разделе "Ввод/кодеки -> Кодеки субтитров -> FPV-OSD".

"Font folder" - папка с шрифтами (см. [fpv-wtf/msp-osd](https://github.com/fpv-wtf/msp-osd/tree/main/fonts))

"Кадры в секунду" - Частота кадров видео. На данный момент нужно задавать вручную.

"Autoload .osd" - Подгружать OSD-файл автоматически. Для автозагрузки файл .osd должен располагаться в одной папке с видеофайлом и иметь такое же имя. Чтобы автозагрузка работала нужно включить модуль "FPV-OSD: OSD on FPV DVR" в разделе "Интерфейс -> Интерфейсы управления".

Также подгружать файл .osd можно вручную через главное меню "Субтитры -> Добавить файл субтитров..." или через командную строку `vlc DJIG0001.mp4 --sub-file=DJIG0001.osd`.

## Ссылки
* https://github.com/fpv-wtf/msp-osd
* https://habr.com/ru/articles/475992/
* https://wiki.videolan.org/Documentation:Documentation/
* https://wiki.videolan.org/Hacker_Guide/Core/
* https://code.videolan.org/videolan/vlc/-/tree/master/doc
