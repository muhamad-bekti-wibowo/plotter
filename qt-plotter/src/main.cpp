#include <QApplication>
#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("EmbeddedPlotter");
    app.setApplicationName("QtPlotter");
    MainWindow w;
    w.show();
    return app.exec();
}
