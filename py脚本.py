texts = ["中文字库", "下载成功", "下载失败", "请选择串口", "正在发送"]
for t in texts:
    esc = ''.join(f'\\x{b:02X}' for b in t.encode('gb2312'))
    print(f'#define STR_{texts.index(t)}  "{esc}"')