using System;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;

namespace HZK12Sender;

public partial class MainWindow : Window
{
    private const int ChunkSize = 128;
    private const int MaxFileSize = 8 * 1024 * 1024;

    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();

        BaudBox.ItemsSource = new[] { "9600", "57600", "115200", "230400" };
        BaudBox.SelectedItem = "115200";

        RefreshPorts();
    }

    // ------------------------------------------------------------------
    // UI 事件
    // ------------------------------------------------------------------

    private void BrowseButton_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "选择 HZK12 二进制字库文件",
            Filter = "二进制文件 (*.bin;HZK12;*)|*.bin;HZK12;*|所有文件 (*.*)|*.*",
            CheckFileExists = true
        };

        if (dlg.ShowDialog(this) == true)
            FilePathBox.Text = dlg.FileName;
    }

    private void RefreshButton_Click(object sender, RoutedEventArgs e) => RefreshPorts();

    private async void SendButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        _busy = true;
        SendButton.IsEnabled = false;
        try
        {
            await SendFileAsync();
        }
        finally
        {
            _busy = false;
            SendButton.IsEnabled = true;
        }
    }

    // ------------------------------------------------------------------
    // 串口刷新 / 日志 / 跨线程 UI 辅助
    // ------------------------------------------------------------------

    private void RefreshPorts()
    {
        string? current = PortBox.SelectedItem as string;

        var names = SerialPort.GetPortNames()
                              .Distinct()
                              .OrderBy(x => x, StringComparer.OrdinalIgnoreCase)
                              .ToArray();

        PortBox.ItemsSource = names;

        if (current is not null && names.Contains(current))
            PortBox.SelectedItem = current;
        else if (names.Length > 0)
            PortBox.SelectedIndex = 0;

        Log("已刷新串口列表。");
    }

    private void Log(string message) => RunOnUi(() =>
    {
        LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
        LogBox.ScrollToEnd();
    });

    private void SetStatus(string text) => RunOnUi(() => StatusText.Text = text);

    private void SetProgress(int value) => RunOnUi(() =>
        TransferProgress.Value = Math.Min(value, TransferProgress.Maximum));

    private void RunOnUi(Action action)
    {
        if (Dispatcher.CheckAccess()) action();
        else Dispatcher.Invoke(action);
    }

    // ------------------------------------------------------------------
    // 发送主流程
    // ------------------------------------------------------------------

    private async Task SendFileAsync()
    {
        string path = FilePathBox.Text;
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
        {
            MessageBox.Show(this, "请先选择有效的字库文件。", "提示",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (PortBox.SelectedItem is not string portName)
        {
            MessageBox.Show(this, "没有可用串口，请连接 USB 转串口并刷新。", "提示",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!int.TryParse(BaudBox.SelectedItem as string, out int baudRate))
        {
            MessageBox.Show(this, "请选择有效的波特率。", "提示",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        byte[] data;
        try
        {
            data = await File.ReadAllBytesAsync(path);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "读取文件失败：" + ex.Message, "错误",
                MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        if (data.Length == 0 || data.Length > MaxFileSize)
        {
            MessageBox.Show(this, "文件大小必须在 1 字节到 8 MiB 之间。", "提示",
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        TransferProgress.Minimum = 0;
        TransferProgress.Maximum = data.Length;
        TransferProgress.Value = 0;

        try
        {
            Log($"文件：{Path.GetFileName(path)}，大小：{data.Length} 字节，CRC32 = 0x{Crc32(data):X8}。");
            // 串口收发放到后台线程，避免阻塞 UI
            await Task.Run(() => Transfer(data, portName, baudRate, Log, SetStatus, SetProgress));

            TransferProgress.Value = data.Length;
            SetStatus("下载完成，整文件 CRC32 校验通过。");
            Log("下载成功，STM32 已确认整文件 CRC32。");

            MessageBox.Show(this, "字库下载成功！", "完成",
                MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            SetStatus("下载失败。");
            Log("错误：" + ex.Message);

            MessageBox.Show(this, ex.Message, "下载失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    // ------------------------------------------------------------------
    // 通信协议实现（在后台线程运行）
    // ------------------------------------------------------------------

    private static void Transfer(
        byte[] data,
        string portName,
        int baudRate,
        Action<string> log,
        Action<string> status,
        Action<int> progress)
    {
        using var sp = new SerialPort(portName, baudRate, Parity.None, 8, StopBits.One)
        {
            ReadTimeout = 100,
            WriteTimeout = 3000,
            DtrEnable = false,
            RtsEnable = false
        };

        sp.Open();
        sp.DiscardInBuffer();
        sp.DiscardOutBuffer();

        status("正在启动 STM32...");
        sp.Write(new byte[] { (byte)'S' }, 0, 1);

        // ---- 12 字节文件头：HZK1 + 长度(4) + CRC32(4) ----
        var header = new byte[12];
        header[0] = (byte)'H';
        header[1] = (byte)'Z';
        header[2] = (byte)'K';
        header[3] = (byte)'1';
        BitConverter.GetBytes((uint)data.Length).CopyTo(header, 4);
        BitConverter.GetBytes(Crc32(data)).CopyTo(header, 8);
        sp.Write(header, 0, header.Length);

        byte ready = ReadExact(sp, 1, 120000)[0];
        if (ready != (byte)'R')
            throw new IOException($"STM32 未准备好，收到 0x{ready:X2}。");

        // ---- 分块传输 ----
        ushort seq = 0;
        int offset = 0;

        while (offset < data.Length)
        {
            int len = Math.Min(ChunkSize, data.Length - offset);

            var packet = new byte[1 + 4 + len + 2];
            packet[0] = 0xA5;
            packet[1] = (byte)seq;
            packet[2] = (byte)(seq >> 8);
            packet[3] = (byte)len;
            packet[4] = (byte)(len >> 8);
            Array.Copy(data, offset, packet, 5, len);

            ushort crc = Crc16Ccitt(packet.AsSpan(1, 4 + len));
            packet[5 + len] = (byte)crc;
            packet[6 + len] = (byte)(crc >> 8);

            bool acked = false;
            for (int retry = 0; retry < 5 && !acked; retry++)
            {
                sp.Write(packet, 0, packet.Length);

                byte[] reply = ReadExact(sp, 3, 5000);
                ushort ackSeq = (ushort)(reply[1] | (reply[2] << 8));

                if (reply[0] == 0x06 && ackSeq == seq)
                {
                    acked = true;
                }
                else if (reply[0] == 0x15)
                {
                    log($"数据包 {seq} 校验失败，重试 {retry + 1}/5。");
                }
                else
                {
                    log($"收到异常应答：{reply[0]:X2}, seq={ackSeq}，重试 {retry + 1}/5。");
                }
            }

            if (!acked)
                throw new IOException($"数据包 {seq} 连续重试失败。");

            offset += len;
            seq++;

            progress(offset);
            status($"已发送 {offset}/{data.Length} 字节 ({offset * 100.0 / data.Length:F1}%)");
        }

        // ---- 结束帧 ----
        sp.Write(new byte[] { 0x5A }, 0, 1);
        byte result = ReadExact(sp, 1, 10000)[0];

        if (result != (byte)'D')
            throw new IOException("STM32 整文件 CRC32 校验失败。");
    }

    // ------------------------------------------------------------------
    // 工具方法
    // ------------------------------------------------------------------

    private static uint Crc32(byte[] data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int i = 0; i < 8; i++)
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return crc ^ 0xFFFFFFFF;
    }

    private static ushort Crc16Ccitt(ReadOnlySpan<byte> data)
    {
        ushort crc = 0xFFFF;
        foreach (byte b in data)
        {
            crc ^= (ushort)(b << 8);
            for (int i = 0; i < 8; i++)
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
        }
        return crc;
    }

    private static byte[] ReadExact(SerialPort port, int count, int timeoutMs)
    {
        var result = new byte[count];
        int got = 0;
        long start = Environment.TickCount64;

        while (got < count)
        {
            if (Environment.TickCount64 - start > timeoutMs)
                throw new TimeoutException($"串口接收超时：需要 {count} 字节，已收到 {got} 字节。");

            try
            {
                int n = port.Read(result, got, count - got);
                got += n;
            }
            catch (TimeoutException)
            {
                // 继续等待，由外层计时判断超时
            }
        }

        return result;
    }
}