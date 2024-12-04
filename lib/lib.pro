QT += core

CONFIG += c++11
#CONFIG += STATIC

TARGET = fitter-gauss-1d
CONFIG += console
CONFIG -= app_bundle

TEMPLATE = lib

DESTDIR = $$_PRO_FILE_PWD_/bin

SOURCES += \
    Gaussian1dFitService.cpp \
    Gauss1d.cpp \
    PolynomialFitter1D.cpp \
    LevMar.cpp

HEADERS += \
    Gaussian1dFitService.h \
    Gauss1d.h \
    HeuristicGauss1dFitter.h \
    PolynomialFitter1D.h \
    LBTAGauss1dFitter.h \
    LMGaussFitter1d.h \
    LevMar.h

win32 {
INCLUDEPATH += C:/msys64/mingw64/include/gsl/
STATIC {
CONFIG += staticlib release
#### static build more linking is needed #####
  LIBS += $$_PRO_FILE_PWD_/../libgsl/libgsl.a
  LIBS += $$_PRO_FILE_PWD_/../libgsl/libgslcblas.a
  LIBS += -lgsl -lgslcblas
} else {
CONFIG += dll release
#### none static build #####
  LIBS += $$_PRO_FILE_PWD_/../libgsl/libgsl-28.dll
  LIBS += $$_PRO_FILE_PWD_/../libgsl/libgslcblas-0.dll
  LIBS += -lgsl -lgslcblas
}
}

unix {
CONFIG += release
LIBS += -lgsl -lgslcblas
}
