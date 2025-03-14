#include "Gaussian1dFitService.h"
#include "HeuristicGauss1dFitter.h"
#include "LBTAGauss1dFitter.h"
#include "LMGaussFitter1d.h"
#include <QElapsedTimer>
#include <QMetaType>
#include <QDir>
#include <iostream>

namespace FitterGauss1d
{
// register metatypes with QT so that we can pass Results and InputParameters via signals/slots
static bool
registerMetaTypes()
{
    std::cerr << "Registering metatype\n";
    std::cerr << "Gaussian1dFitService::Results = "
              << qRegisterMetaType < FitterGauss1d::ResultsG1dFit > ( "ResultsG1dFit" ) <<
        "\n";
    std::cerr << "Gaussian1dFitService::InputParameters = "
              << qRegisterMetaType < FitterGauss1d::InputParametersG1dFit > (
        "InputParametersG1dFit" )
              << "\n";
    return true;
}

static bool initOnce_ = registerMetaTypes();

// private exception class
class Interrupt
{ };

ResultsG1dFit::ResultsG1dFit()
{
    //    std::cerr << "Results constructor from thread " << QThread::currentThread () << "\n";
    status_ = Error;
    diffSq = 0 / 0.0;
    rms = 0 / 0.0;
}

/*

Results::Results(const Results & r)
{
    std::cerr << "Results copy-constructor " << r.testImage.width ()
              << "x" << r.testImage.height () << " from thread " << QThread::currentThread () << "\n";

    testImage = r.testImage;
}
*/

Manager::Manager( QObject * parent ) :
    QObject( parent ),
    worker_( interruptRequested_=false )
{
    // initialize internal fields
    interruptRequested_ = false;
    hasPendingInput_ = false;
    workerBusy_ = false;

    // connect worker and manager
    connect( & worker_, SIGNAL( progress( ResultsG1dFit ) ),
             this, SLOT( onProgress( ResultsG1dFit ) ) );
    connect( & worker_, SIGNAL( done( ResultsG1dFit ) ),
             this, SLOT( onDone( ResultsG1dFit ) ) );
    connect( & worker_, SIGNAL( error( QString ) ),
             this, SLOT( onError( QString ) ) );
    connect( & worker_, SIGNAL( interrupt() ),
             this, SLOT( onInterrupt() ) );
    connect( this, SIGNAL( privateGo( InputParametersG1dFit ) ),
             & worker_, SLOT( onGoFromService( InputParametersG1dFit ) ) );

    // move the worker to a separate thread
    worker_.moveToThread( & workerThread_ );
    workerThread_.start();
}

void
Manager::request( InputParametersG1dFit input )
{
    //    std::cerr << "Manager::request " << input.toString() << "\n";

    // if worker is running, ask for interrupt
    if ( workerBusy_ ) {
        interruptRequested_ = true;
    }
    pendingInput_ = input;
    hasPendingInput_ = true;
    schedulePending();
}

void
Manager::stop()
{
   workerThread_.quit();
}

// if there is a pending request, and worker is not busy, ask worker
// to process the pending request
// otherwise do nothing
void
Manager::schedulePending()
{
    if ( ! hasPendingInput_ ) {
        return;
    }
    if ( workerBusy_ ) {
        return;
    }

    hasPendingInput_ = false;
    interruptRequested_ = false;
    workerBusy_ = true;

    // tell worker to start working on the request
    emit privateGo( pendingInput_ );
}

Worker::Worker( bool & interruptFlagRef )
    : interruptFlag_( interruptFlagRef )
{ }

Worker::~Worker()
{ }

// throws an exception if the interrupt flag is set
void
Worker::checkForInterrupts()
{
    if ( interruptFlag_ == true ) {
        throw Interrupt();
    }
}

void
Worker::onGoFromService( InputParametersG1dFit input )
{
    try {
        doWork( input );
    }
    catch ( Interrupt & ) {
        // std::cerr << "*** Worker interrupted ***\n";
        emit interrupt();
    }
    catch ( std::runtime_error & e ) {
        qDebug() << "Worker runtime-exception: " << e.what();
        emit error( QString( "runtime-exception: %1" ).arg( e.what() ) );
    }
    catch ( std::exception & e ) {
        qDebug() << "Worker std-exception: " << e.what();
        emit error( QString( "std-exception: %1" ).arg( e.what() ) );
    }
    catch ( std::string & s ) {
        qDebug() << "Worker exception: " << s.c_str();
        emit error( QString( "exception: %1" ).arg( s.c_str() ) );
    }
    catch ( QString & s ) {
        qDebug() << "Worker exception: " << s;
        emit error( QString( "exception: %1" ).arg( s ) );
    }
    catch ( char * s ) {
        qDebug() << "Worker exception: " << s;
        emit error( QString( "exception: %1" ).arg( s ) );
    }
    catch ( ... ) {
        qDebug() << "Worker uncaught exception...";
        emit error( "Uncaught exception" );
    }
    qDebug() << "Exiting onGoFromService";
} // Worker::onGoFromService

void
Worker::doWork( InputParametersG1dFit input )
{
    // prepare results (partial)
    ResultsG1dFit res;
    res.input = input;
    res.status_ = ResultsG1dFit::Partial;
    res.params.resize( res.input.nGaussians * 3 + res.input.poly, 0.0 );

    // handle null input (early exit)
    if ( input.isNull || input.nGaussians + input.poly <= 0 ) {
        res.status_ = ResultsG1dFit::Complete;
        res.diffSq = - 1;
        emit done( res );
        return;
    }

    // should we do all fitters?
    // heuristic fitter only if initial guess not present
    bool doHeuristic = ( int ( input.initGuess.size() ) != input.nGaussians * 3 + input.poly );

    // threshold accepting only if
    //   - multiple gaussians
    //   - or 1 gaussian and at least 1 polynomial term
    //   - it is enabled by the user
    //
    bool doLBTA = input.nGaussians > 1 || ( input.nGaussians == 1 && input.poly > 0 );
    if ( ! input.randomHeuristicsEnabled ) {
        doLBTA = false;
    }

    // levenberg/marquardt if there is at least 1 gaussian
    bool doLM = input.nGaussians > 0;

    //    doLBTA = false;
    //    doLM = false;

    // initialize timer for reporting progress
    QElapsedTimer progressTimer;
    progressTimer.restart();
    bool firstProgressNeeded = true;

    int iteration = 0;

    // set up the data source for the various gaussian fitters
    FitterGauss1d::FitterInput dataInterface( input.data );
    dataInterface.x1 = input.left;
    dataInterface.x2 = input.right;
    dataInterface.nGaussians = input.nGaussians;
    dataInterface.nPolyTerms = input.poly;
    dataInterface.precomputeRangeMinMax();
    dataInterface.ranges.resize( dataInterface.numParams() );
    double range12 = dataInterface.rangeMax - dataInterface.rangeMin;
    for ( int i = 0 ; i < dataInterface.nGaussians ; i++ ) {
        // center
        dataInterface.ranges[i * 3 + 0].set( input.left, input.right );

        // amplitude
        dataInterface.ranges[i * 3 + 1].set( dataInterface.rangeMin - 0.1 * range12,
                                             dataInterface.rangeMax + 0.1 * range12 );
        qDebug() << "Data range:" << dataInterface.rangeMin << dataInterface.rangeMax;

        //        dataInterface.ranges[i*3+1].set( -10, 10);
        // variance controlling term
        dataInterface.ranges[i * 3 + 2].set( - 1.0 / ( 2 * 0.25 ),
                                             - 1.0 /
                                             ( 2 * ( dataInterface.x2 - dataInterface.x1 ) *
                                               ( dataInterface.x2 - dataInterface.x1 ) ) );
    }

    // ----------------------------------------------------------------------
    // run the heuristic fitter
    // ----------------------------------------------------------------------
    if ( doHeuristic ) {
        FitterGauss1d::HeuristicFitter hFitter( dataInterface );

        // start fitting
        iteration = 0;
        while ( true ) {
            iteration++;
            checkForInterrupts();
            bool fitDone = hFitter.iterate();

            // report progress:
            //   - after 100ms if it's the first time
            //   - after that every 1000ms
            bool reportProgress = progressTimer.elapsed() > 1000 ||
                                  ( progressTimer.elapsed() > 100 && firstProgressNeeded );
            bool copyResults = fitDone || reportProgress;
            if ( copyResults ) {
                res.params = hFitter.getResults();
                res.diffSq = dataInterface.calculateDiffSq( res.params );
                res.rms = sqrt( res.diffSq / dataInterface.data.size() );
                if ( int ( res.params.size() ) != input.nGaussians * 3 + input.poly ) {
                    throw std::runtime_error(
                              "Gaussian1dFitService::Worker - mismatch in result size" );
                }
            }

            if ( reportProgress ) {
                firstProgressNeeded = false;
                progressTimer.restart();
                res.status_ = res.Partial;
                res.info = QString( "Heuristics #%1" ).arg( iteration );
                checkForInterrupts();
                emit progress( res );
            }

            if ( fitDone ) {
                break;
            }
        }
        qDebug() << "Heuristic fitter produced diffSq = " << res.diffSq;
    }
    else {
        // if caller provided initial guess, use that instead
        res.params = input.initGuess;
        res.diffSq = dataInterface.calculateDiffSq( res.params );
        res.rms = sqrt( res.diffSq / dataInterface.data.size() );
        res.status_ = res.Partial;
        res.info = QString( "Initial guess" );
        qDebug() << "Initial guess has diffSq = " << res.diffSq;
    }

    // ----------------------------------------------------------------------
    // setup the LBTA fitter
    // ----------------------------------------------------------------------
    if ( doLBTA ) {
        FitterGauss1d::LBTAFitter taFitter( dataInterface );
        taFitter.setInitialParams( res.params );

        // start fitting
        iteration = 0;
        while ( true ) {
            iteration++;
            checkForInterrupts();

            //        dbg(1) << "Calling taFitter.iterate()\n";
            bool taDone = taFitter.iterate();

            // report progress:
            //   - after 100ms if it's the first time
            //   - after that every 1000ms
            bool reportProgress = progressTimer.elapsed() > 1000 ||
                                  ( progressTimer.elapsed() > 100 && firstProgressNeeded );
            bool copyResults = taDone || reportProgress;
            if ( copyResults ) {
                res.params = taFitter.getResults();
                res.diffSq = dataInterface.calculateDiffSq( res.params );
                res.rms = sqrt( res.diffSq / dataInterface.data.size() );
                if ( int ( res.params.size() ) != input.nGaussians * 3 + input.poly ) {
                    throw std::runtime_error(
                              "Gaussian1dFitService::Worker - mismatch in result size" );
                }
            }

            if ( reportProgress ) {
                firstProgressNeeded = false;
                progressTimer.restart();
                res.status_ = res.Partial;
                res.info = QString( "TA #%1" ).arg( iteration );
                checkForInterrupts();
                emit progress( res );
            }

            if ( taDone ) {
                break;
            }
        }
        qDebug() << "LBTA fitter produced diffSq = " << res.diffSq;
    }

    if ( doLM ) {
        // ----------------------------------------------------------------------
        // setup up the lev-mar fitter
        // ----------------------------------------------------------------------
        FitterGauss1d::LMFitter lmfitter( dataInterface );
        lmfitter.setInitialParams( res.params );

        // start fitting
        iteration = 0;
        while ( true ) {
            iteration++;
            checkForInterrupts();
            bool lmDone = lmfitter.iterate();

            // report progress:
            //   - after 100ms if it's the first time
            //   - after that every 1000ms
            bool reportProgress = progressTimer.elapsed() > 1000 ||
                                  ( progressTimer.elapsed() > 100 && firstProgressNeeded );
            bool copyResults = lmDone || reportProgress;
            if ( copyResults ) {
                res.params = lmfitter.getResults();
                res.diffSq = dataInterface.calculateDiffSq( res.params );

                // TODO: fix up RMS to account for NANs
                res.rms = sqrt( res.diffSq / dataInterface.data.size() );
                if ( int ( res.params.size() ) != input.nGaussians * 3 + input.poly ) {
                    throw std::runtime_error(
                              "Gaussian1dFitService::Worker - mismatch in result size" );
                }
            }

            if ( reportProgress ) {
                firstProgressNeeded = false;
                progressTimer.restart();
                res.status_ = res.Partial;
                res.info = QString( "LevMar #%1" ).arg( iteration );
                checkForInterrupts();
                emit progress( res );
            }

            if ( lmDone ) {
                break;
            }
        }
        qDebug() << "LevMar iteration = " << iteration;
        qDebug() << "LevMar fitter produced diffSq = " << res.diffSq;

//        {
//            QString s;
//            QTextStream out(&s);
//            for( size_t i = 0 ; i < res.params.size() ; i ++ ) {
//                if( i > 10) { out << "..."; break; }
//                out << res.params[i] << " ";
//            }
//            out << "\n";
//            dbg(1) << "   params: " << s;
//        }
    }

    res.status_ = res.Complete;
    res.info = "All done.";
    checkForInterrupts();
    emit done( res );
} // Worker::doWork

void
Manager::onProgress( ResultsG1dFit r )
{
    //    std::cerr << "Manager::onProgress: " << r.toString() << "\n";
    emit progress( r );
    emit results( r );
}

void
Manager::onDone( ResultsG1dFit r )
{
    //    std::cerr << "Manager::onDone: " << r.toString() << "\n";
    emit done( r );
    emit results( r );
    workerBusy_ = false;
    schedulePending();
}

void
Manager::onError( QString s )
{
    //    std::cerr << "Manager::onError: " << s << "\n";
    emit error( s );
    workerBusy_ = false;
    schedulePending();
}

void
Manager::onInterrupt()
{
    //    std::cerr << "Manager::onInterrupt\n";
    workerBusy_ = false;
    schedulePending();
}

//double Worker::getData( int x)
//{
//    if( x < 0 || x >= int(data.size()) ) {
//        LTHROW( QString("Gaussian1dFitService::Worker::getData() out of bounds x = %1").arg(x));
//        qDebug() << "Gaussian1dFitService::Worker::getData() out of bounds x = " << x << "\n";
//        return 0;
//    }
//    return data[x];
//}
} // namespace Gaussian1dFitService
