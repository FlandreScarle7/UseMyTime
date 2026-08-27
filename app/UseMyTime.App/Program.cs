/*
 * UseMyTime - 程序入口
 */
namespace UseMyTime.App;

internal static class Program
{
    [STAThread]
    static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
    }
}
