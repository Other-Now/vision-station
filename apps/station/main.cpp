// vs_station: the Qt operator GUI.
//
//   vs_station                         interactive
//   vs_station --smoke live|inspect --seconds 4 --screenshot out.png
//       headless self-test (use QT_QPA_PLATFORM=offscreen): connects to the
//       rig, runs, saves a screenshot, exits 0 only if frames were painted.
#include <QApplication>
#include <QTimer>
#include <cstdio>

#include "main_window.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QString smoke, shot;
  double seconds = 4;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == "--smoke" && i + 1 < args.size()) smoke = args[++i];
    else if (args[i] == "--seconds" && i + 1 < args.size()) seconds = args[++i].toDouble();
    else if (args[i] == "--screenshot" && i + 1 < args.size()) shot = args[++i];
  }
  MainWindow w;
  w.resize(1180, 720);
  w.show();
  if (smoke.isEmpty()) return app.exec();

  const bool started = smoke == "inspect" ? w.start_inspection() : w.start_live();
  if (!started) {
    std::fprintf(stderr, "smoke: start failed\n");
    return 1;
  }
  QTimer::singleShot(static_cast<int>(seconds * 1000), &w, [&] {
    if (!shot.isEmpty()) w.grab().save(shot);
    std::printf("smoke %s: %llu frames painted\n", qPrintable(smoke), static_cast<unsigned long long>(w.frames_shown()));
    app.exit(w.frames_shown() > 0 ? 0 : 1);
  });
  return app.exec();
}
