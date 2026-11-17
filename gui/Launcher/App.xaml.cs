using System;
using System.IO;
using System.Threading.Tasks;
using System.Windows;

namespace TdoLauncher;

public partial class App : Application
{
    public static string ErrorLogPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "3DO to PC", "launcher-error.txt");

    public App()
    {
        // Never fail silently: show the error and keep a copy to send.
        DispatcherUnhandledException += (_, e) => { Report(e.Exception); e.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Report(e.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, e) => { Report(e.Exception); e.SetObserved(); };
    }

    public static void Report(Exception? ex)
    {
        string text = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss}  3DO Launcher error{Environment.NewLine}{ex}{Environment.NewLine}{Environment.NewLine}";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ErrorLogPath)!);
            File.AppendAllText(ErrorLogPath, text);
        }
        catch { }
        MessageBox.Show($"The launcher hit an error:\n\n{ex?.GetBaseException().Message}\n\nDetails were saved to:\n{ErrorLogPath}",
                        "3DO Launcher", MessageBoxButton.OK, MessageBoxImage.Error);
    }
}
