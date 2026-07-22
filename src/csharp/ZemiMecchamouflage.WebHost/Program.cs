using ZemiMecchamouflage.Controller;

namespace ZemiMecchamouflage.WebHost;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        var paths = new ZemiMecchamouflage.Core.AppPaths(VersionInfo.Current);
        DiagnosticsState.Initialize(paths, VersionInfo.Current);
#if MECCHA_RESEARCH_BUILD
        if (ResearchRunner.IsRequested(args))
        {
            Environment.ExitCode = ResearchRunner.RunAsync(args).GetAwaiter().GetResult();
            return;
        }
#endif
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, args) => DiagnosticsState.RecordException("winforms_thread_exception", args.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception exception)
                DiagnosticsState.RecordException("appdomain_unhandled_exception", exception);
        };
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            DiagnosticsState.RecordException("task_unobserved_exception", args.Exception);
            args.SetObserved();
        };

        try
        {
            DiagnosticsState.SetStartupPhase("application_configuration");
            ApplicationConfiguration.Initialize();
            DiagnosticsState.SetStartupPhase("main_form_create");
            using var form = new MainForm(new HostSession(VersionInfo.Current, ReadDiagnosticStrokeLimit(args)));
            DiagnosticsState.SetStartupPhase("application_run");
            Application.Run(form);
            DiagnosticsState.SetStartupPhase("application_exit");
        }
        catch (Exception exception)
        {
            DiagnosticsState.RecordException("application_run_failed", exception);
            MessageBox.Show(
                "Zemi Mecchamouflage failed to start. Diagnostic logs were written to:" +
                Environment.NewLine + paths.DiagnosticsDirectory +
                Environment.NewLine + Environment.NewLine + exception.Message,
                "Zemi Mecchamouflage",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
    }

    private static int ReadDiagnosticStrokeLimit(string[] args)
    {
        for (var index = 0; index < args.Length; ++index)
        {
            if (!string.Equals(args[index], "--diagnostic-stroke-limit", StringComparison.Ordinal))
                continue;
            if (++index >= args.Length ||
                !int.TryParse(args[index], out var value) ||
                value is < 1 or > 10_000)
            {
                throw new ArgumentException("--diagnostic-stroke-limit must be an integer from 1 through 10000.");
            }
            return value;
        }
        return 0;
    }
}
