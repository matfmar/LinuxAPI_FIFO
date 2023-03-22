/*Projekt 2, autor: Mateusz Marzec
 * program zakłada, że oba procesy zostaną uruchomione z tymi samymi parametrami
 * (choć mogą być podane w niewłaściwej kolejności).
*/

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#define BUFFER_SIZE PIPE_BUF

struct ArgStruct {  //argumenty dla wątków
    int fildesIn;
    int fildesOut;
    int fildesCtlIn;
    int fildesCtlOut;
    int fildesShared;
};

sem_t sem, semError;
int flagError = 0;  //błędy między wątkami
int globalCtlOut;
bool sluchanieKom = true;
bool wysylanie = true;
bool master = false;
void deleteTempFiles();

void signal_handler(int signo) {
	if (signo == SIGINT || signo == SIGTERM || signo == SIGSTOP) {
		deleteTempFiles();
		exit(EXIT_FAILURE);
	}
}

void Blad(const char* str) {    //wyrzucanie błędów na fildes 2
    sem_wait(&sem);
    sem_wait(&semError);
    flagError = 1;
    sem_post(&semError);
    perror(str);
    sem_post(&sem);
}

void* writingTh(void* arg) {    //wątek odczytujący dane z klawiatury i piszący komunikaty do FIFO
	struct ArgStruct* argsPtr = (struct ArgStruct*) arg;
	int fildes = argsPtr -> fildesOut;  //tu wysyła komunikaty
	int fildesShared = argsPtr -> fildesShared; //tu wysyła dane na ekran
	int fildesCtlOut = argsPtr -> fildesCtlOut;   //tu wysyła info, że koniec z komunikatami
    char textBuf[BUFFER_SIZE+1];    //do odbioru z klawiatury i wysyłki do FIFO
    char textContinue = 'n';     //kontrolka z uzytkownikiem
    char ctlBuf = 'n' ;   //to jest wysyłane gdy koniec z komunikatami
	int nRead, nWritten, result, resShared;
	bool ctrlD2 = false;    //koniec wysylki
	bool koniecSluchanieKomInt = false;   //koniec sluchania
	bool koniecSluchanieCtlInt = false;
	int myFlagError;   //oszczedza czas
	int timeCounter = 0;
	bool timeEnded = false;     //czas plynie...
    struct timeval timeout;
    fd_set inputs, testInputs;

	memset(textBuf, '\0', sizeof(textBuf)); //wypełnienie bufora odczytu z klawiatury

    FD_ZERO(&inputs);
    FD_SET(0, &inputs);  //wypelnienie setu na inputach wejsciem z klawiatury

    sem_wait(&sem);
	dprintf(fildesShared, "UWAGA\nJesli chcesz zakonczyc sluchanie komunikatow, wpisz '$' jako pierwszy znak\n");  //info poczatkowe dla usera
	dprintf(fildesShared, "Jesli chcesz zakonczyc pisanie komunikatow, wpisz '!' jako pierwszy znak\n");
	sem_post(&sem);
    while (true) {
        testInputs = inputs;    //odnowienie setu
        timeout.tv_sec = 30;    //odnowienie timelapsu
        timeout.tv_usec = 0;

        sem_wait(&semError);
        myFlagError = flagError;
        if (!wysylanie) {   //jesli globalna flaga zapisu zmienila sie, trzeba zmienic zawczasu prompt
            sem_post(&semError);
            ctrlD2 = true;
            sem_wait(&sem);
            dprintf(fildesShared, "Zakonczono wysylanie komunikatow.\n");
            sem_post(&sem);
        }
        else
            sem_post(&semError);
        sem_wait(&semError);
        if (!sluchanieKom) {
            sem_post(&semError);
            koniecSluchanieKomInt = true;
            sem_wait(&sem);
            dprintf(fildesShared, "Zakonczono odbior komunikatow.\n");
            sem_post(&sem);
        }
        else
            sem_post(&semError);

        if (ctrlD2 && koniecSluchanieKomInt) {    //ani nie sluchamy, ani nie piszemy
            sem_wait(&sem);
            dprintf(fildesShared, "Poprawny koniec watka piszacego\n");
            sem_post(&sem);
            sem_wait(&semError);
            sluchanieKom = false;
            sem_post(&semError);
            pthread_exit("Poprawnie zakonczono dzialanie watka piszacego\n");
        }

        if (myFlagError != 0) {
            sem_wait(&sem);
            dprintf(fildesShared, "Wtorne zamkniecie watka piszacego\n");
            sem_post(&sem);
            pthread_exit("Watek piszacy wtornie zamkniety\n");
        }

        sem_wait(&sem);
        if (!ctrlD2)    //albo komunikat albo przypominajka o mozliwosci zakonczenia sluchanki
            dprintf(fildesShared, "Twoj komunikat:\n");
        else
            dprintf(fildesShared, "Zakonczono wysylke. Wpisz '$' jako pierwszy znak by zakonczyc komunikacje lub cokolwiek innego by ja przedluzyc:\n");
        sem_post(&sem);

        result = select(10, &testInputs, (fd_set*) NULL, (fd_set*) NULL, &timeout);     //oczekiwanie na input z klawiatury...
        switch (result) {
            case -1:
                Blad("blad select(): ");
                pthread_exit("blad watka zapisu (odczyt z klawiatury)\n");
                break;
            case 0:     //gdy timelapse uplynie
                timeCounter++;  //licznik limitu lenistwa
                sem_wait(&sem);
                dprintf(fildesShared, "Napisz cos, bo sie wylacze!\n");    //przypominamy userowi
                sem_post(&sem);
                break;
            default:
                timeCounter = 0;    //po wpisaniu czegokolwiek odnawiamy licznik czasu
                if (FD_ISSET(0, &testInputs)) {
                    nRead = read(0, textBuf, BUFFER_SIZE);
                    if (nRead <= 0) {	//ten warunek obejmuje ctrl+D
                        Blad("blad odczytu z klawiatury: ");
                        pthread_exit("blad watka zapisu (odczyt z klawiatury)\n");
                    }
                    sem_wait(&sem);
                    dprintf(fildesShared, "odczyt z klawiatury OK\n");
                    sem_post(&sem);
                    if (textBuf[0] == '$') {    //wcisnieto koniec sluchanki
                        if (!koniecSluchanieKomInt) {       //zeby nie nacisnac 2 razy
                            koniecSluchanieKomInt = true; //zmienna wewnetrzna
                            sem_wait(&semError);
                            sluchanieKom = false;  //flaga globalna
                            sem_post(&semError);
                            ctlBuf = 'r';
                            nWritten = write(fildesCtlOut, &ctlBuf, 1);  //info do drugiego procesu
                            if (nWritten == -1) {
                                Blad("Blad komunikacji z drugim procesem: ");
                                pthread_exit("Blad watka zapisu (komunikacja IPC)\n");
                            }
                            sem_wait(&sem);
                            dprintf(fildesShared, "Zakonczono sluchanie drugiego procesu\nWyslano informacje do drugiego procesu\n");
                            sem_post(&sem);
                        }
                    }
                    else if (textBuf[0] == '!' && !ctrlD2) {    //koniec pisania
                        if (!ctrlD2) {      //zeby nie nacisnac 2 razy
                            ctrlD2 = true;      //zmienna wewnetrzna
                            sem_wait(&semError);
                            wysylanie = false;  //flaga globalna
                            sem_post(&semError);
                            ctlBuf = 'w';
                            nWritten = write(fildesCtlOut, &ctlBuf, 1);  //wyslanie info do drugiego procesu
                            if (nWritten == -1) {
                                Blad("Blad wyslania informacji kontrolnej do drugiego procesu: ");
                                pthread_exit("blad watka zapisu (kontrol do FIFO)\n");
                            }
                            sem_wait(&sem);
                            dprintf(fildesShared, "Zakonczono wysylanie komunikatow\nWyslano informacje do drugiego procesu\n");
                            sem_post(&sem);
                        }
                    }
                    else {  //wcisnieto normalny komunikat
                        if (!ctrlD2) {   //jak koniec wysylki komunikatow to analiza tekstu nie ma sensu
                            sem_wait(&semError);
                            myFlagError = flagError;
                            if (wysylanie) {    //w ostatniej chwili globalna flaga zapisu mogla ulec zmianie
                                sem_post(&semError);
                                if (myFlagError != 0) {
                                    sem_wait(&sem);
                                    dprintf(fildesShared, "Wtorne zamkniecie watka piszacego\n");
                                    sem_post(&sem);
                                    pthread_exit("Watek piszacy wtornie zamkniety\n");
                                }
                                nWritten = write(fildes, textBuf, BUFFER_SIZE);
                                if (nWritten == -1) {
                                    Blad("blad zapisu do FIFO: ");
                                    pthread_exit("blad watka zapisu (zapis do FIFO)\n");
                                }
                                sem_wait(&sem);
                                dprintf(fildesShared, "wyslano do FIFO\n");
                                sem_post(&sem);
                            }
                            else {
                                sem_post(&semError);
                                ctrlD2 = true;
                                sem_wait(&sem);
                                dprintf(fildesShared, "Zakonczono wysylanie komunikatow.\n");
                                sem_post(&sem);
                            }
                        }
                    }
                    memset(textBuf, '\0', nRead+1); //odnowienie bufora odczytu z klawiatury
                }
                break;
        }
        if (timeCounter > 3) {  //przekroczenie limitu lenistwa
            sem_wait(&sem);
            sem_wait(&semError);
            flagError = 2;
            sem_post(&semError);
            dprintf(fildesShared, "Uplynal czas na wejscia z klawiatury\n");
            sem_post(&sem);
            pthread_exit("Zakonczono dzialanie watka piszacego (brak wejscia z klawiatury)\n");
        }
    }
    pthread_exit(NULL); //dla poprawnosci semantycznej kodu funkcji; ta linijka nigdy nie nastapi
}

void* readingTh(void* arg) {
	struct ArgStruct* argsPtr = (struct ArgStruct*) arg;
	int fildes = argsPtr -> fildesIn;  //stąd odbiera komunikaty
	int fildesShared = argsPtr -> fildesShared; //tu wysyła dane na ekran
	int fildesCtlIn = argsPtr -> fildesCtlIn;   //stąd obiera info, że nie ma już co odbierać
    char textBuf[BUFFER_SIZE+1];    //do odbioru z FIFO i wysyłki na ekran
    char ctlBuf = 'n';  //do odbioru info z drugiego procesu
	int nRead, nWritten, result, resShared;
    fd_set inputs, testInputs;
    bool koniecSluchanieKomInt = false;
    bool koniecSluchanieCtlInt = false;
    bool ctrlD2 = false;

	memset(textBuf, '\0', sizeof(textBuf)); //wypełnienie bufora odczytu z klawiatury

    FD_ZERO(&inputs);
    FD_SET(fildes, &inputs);    //odczyt z FIFO
    FD_SET(fildesCtlIn, &inputs);   //odczyt z FIFO - kontrolne komunikaty

    while (true) {
        testInputs = inputs;

        sem_wait(&semError);
        koniecSluchanieKomInt = !sluchanieKom;
        ctrlD2 = !wysylanie;
        sem_post(&semError);

        if (koniecSluchanieKomInt && ctrlD2) {
            sem_wait(&sem);
            dprintf(fildesShared, "Nacisnij ENTER\n");
            sem_post(&sem);
        }

        result = select(10, &testInputs, (fd_set*) NULL, (fd_set*) NULL, NULL); //czekamy na wejscie z FIFO lub kontroli procesu
        switch (result) {
            case -1:
                Blad("blad select(): ");
                pthread_exit("blad watka odczytu\n");
                break;
            default:
                if (!koniecSluchanieKomInt && FD_ISSET(fildes, &testInputs)) {    //wejscie ze zwyklego FIFO
                    nRead = read(fildes, textBuf, BUFFER_SIZE);
                    if (nRead == -1) {
                        Blad("blad odczytu z FIFO ");
                        pthread_exit("blad watka odczytu (odczyt z FIFO)\n");
                    }
                    sem_wait(&sem);
                    printf("----------\n");
                    printf("Wiadomosc: %s", textBuf); //wydruk na klasyczny fildes 1
                    printf("----------\n");
                    sem_post(&sem);
                    memset(textBuf, '\0', nRead+1);
                }
                if (FD_ISSET(fildesCtlIn, &testInputs)) {   //wejscie z FIFO przezn. do kontroli
                    nRead = read(fildesCtlIn, &ctlBuf, 1);
                    if (nRead == -1) {
                        Blad("blad odczytu z FIFO ");
                        pthread_exit("blad watka odczytu (odczyt z FIFO)\n");
                    }
                    if (ctlBuf == 'r') {    //ten sygnal oznacza zahamowanie odczytu u zrodla => nie ma sensu pisac
                        ctrlD2 = true;
                        sem_wait(&semError);
                        wysylanie = false;
                        sem_post(&semError);
                        sem_wait(&sem);
                        dprintf(fildesShared, "Drugi proces zablokowal odczyt. Zahamowano wysylanie komunikatow.\n");
                        sem_post(&sem);
                    }
                    else if (ctlBuf == 'w') {   //ten sygnal oznacza zahamowanie pisania u zrodla => nie ma sensu dalej sluchac komunikatow
                        koniecSluchanieKomInt = true;
                        sem_wait(&semError);
                        sluchanieKom = false;
                        sem_post(&semError);
                        FD_CLR(fildes, &inputs);
                        sem_wait(&sem);
                        dprintf(fildesShared, "Drugi proces przestal pisac komunikaty. Zahamowano odczyt z FIFO.\nSlucham tylko info kontrolnych\n");
                        sem_post(&sem);
                    }
                    else if (ctlBuf == 'x') {
						koniecSluchanieKomInt = true;
						sem_wait(&semError);
						sluchanieKom = false;
						wysylanie = false;
						sem_post(&semError);
						ctrlD2 = true;
						FD_CLR(fildes, &inputs);
						sem_wait(&sem);
						dprintf(fildesShared, "Drugi proces zakonczyl przedwczesnie dzialalnosc.\n");
						sem_post(&sem);
					}
                    ctlBuf = 'n';
                }
                break;
        }
    }
    pthread_exit(NULL); //ta linijka nigdy nie nastapi
}

bool checkIfFifo(int fildes) {
    struct stat fileStat;
    if (fstat(fildes, &fileStat) == -1)
        return false;
    return (S_ISFIFO(fileStat.st_mode));
}

void deleteTempFiles() {		//funkcja sprzatajaca przed zamknieciem
	int res,res1,res2;
	char finalChar = 'x';
	res = write(globalCtlOut, &finalChar, 1);
	if (res == -1) 
		printf("Nie udalo sie wyslac wiadomosci do drugiego procesu o zakonczeniu dzialalnosci\n");
	else
		printf("Wyslano do drugiego procesu informacje o zakonczeniu dzialanosci.\n");
	if (!master)	//usuwa tylko ten co je stworzyl
		return;
	res = remove("/tmp/mmfifoout");
	if (res == -1) 
        printf("Nie udalo sie usunac pliku /tmp/mmfifoout\n");
	res1 = remove("/tmp/mmfifoin");
	if (res1 == -1)
       printf("Nie udalo sie usunac pliku /tmp/mmfifoin\n");
	res2 = remove("/tmp/mmfifolock");
	if (res2 == -1)
       printf("Nie udalo sie usunac pliku /tmp/mmfifolock\n");
    if ((res != -1 && res1 != -1) && res2 != -1)
		printf("Usunieto pliki tymczasowe\n");
}

void deleteSemaphores() {
	sem_destroy(&sem);
	sem_destroy(&semError);
}

int main(int argc, char* argv[]) {

    if (argc != 3) {
        printf("Niewlasciwa liczba argumentow!\n");
        exit(EXIT_FAILURE);
    }
    if (signal(SIGINT, signal_handler) == SIG_ERR)
		printf("Nie moze zostac wylapany sygnal zamykajacy\n");
    
	printf("Witaj w programie do komunikacji miedzyprocesowej\n");

	char* fifoIn,* fifoOut,* fifoCtlIn,* fifoCtlOut;   //łącznie używamy czterech FIFO, dwa jawnie
    int res, res1, res2;
    int fildesLOCK, fildesTemp;
    int fildesIn, fildesOut, fildesCtlIn, fildesCtlOut, fildesShared;

    fildesLOCK = open("/tmp/mmfifolock", O_RDWR | O_CREAT | O_EXCL, 0444);  //ktory proces pierwszy utworzy file lock, tak ulozone beda FIFO
    if (fildesLOCK != -1) {     //ten co, utworzyl file lock
		master = true;
        if (access("/tmp/mmfifoout", F_OK) != 0) {		//tmp fifo dla ctl Out
			res = mkfifo("/tmp/mmfifoout", 0777);
			if (res != 0) {
				perror("blad tworzenia fifo: ");
				exit(EXIT_FAILURE);
			}
		}
		fifoCtlOut = "/tmp/mmfifoout";
		if (access("/tmp/mmfifoin", F_OK) != 0) {		//tmp fifo dla ctl in
			res = mkfifo("/tmp/mmfifoin", 0777);
			if (res != 0) {
				perror("blad tworzenia fifo: ");
				exit(EXIT_FAILURE);
			}
		}
		fifoCtlIn = "/tmp/mmfifoin";
		if (access(argv[1], F_OK) != 0) {
			printf("Podany plik %s nie istnieje. Zostanie utworzony.", argv[1]);
			res = mkfifo(argv[1], 0777);
			if (res != 0) {
				perror("blad tworzenia fifo: ");
				exit(EXIT_FAILURE);
			}
		}
		fifoIn = argv[1];
		if (access(argv[2], F_OK) != 0) {
			printf("Podany plik %s nie istnieje. Zostanie utworzony.", argv[2]);
			res = mkfifo(argv[2], 0777);
			if (res != 0) {
				perror("blad tworzenia fifo: ");
				exit(EXIT_FAILURE);
			}
		}
		fifoOut = argv[2];
		int fildesTemp = open(fifoCtlOut, O_RDWR);	//wyslanie informacji ktory plik jest ktory
		if (fildesTemp == -1) {
			perror("blad otwarcia fifo kom.: ");
			exit(EXIT_FAILURE);
		}
		char* infoFifo = fifoIn;	//wysylana jest nazwa pliku odbioru u mastera
		res = write(fildesTemp, infoFifo, strlen(infoFifo));
		if (res == -1) {
			perror("blad wyslania fifo kom.: ");
			exit(EXIT_FAILURE);
		}
		printf("Wyslano do drugiego procesu informacje o kolejnosci FIFO\n");
    }
    else {		//proces ktory nie zdazyl utworzyc file locka (spoznil sie)
		printf("Ladowanie w toku...\n");
		sleep(2);	//czas na dokonczenie tworzenia plikow przez pierwszy proces
		fifoCtlIn = "/tmp/mmfifoout";	//na odwrot
		fifoCtlOut = "/tmp/mmfifoin";
		fildesTemp = open(fifoCtlIn, O_RDWR);
		if (fildesTemp == -1) {
			perror("blad otwarcia fifo kom.");
			exit(EXIT_FAILURE);
		}
		char bufRead[BUFFER_SIZE+1];
		memset(bufRead, '\0', sizeof(bufRead));
		res = read(fildesTemp, bufRead, BUFFER_SIZE);
		if (res == -1) {
			perror("blad odczytu z fifo kom: ");
			exit(EXIT_FAILURE);
		}
		printf("Odczytano nazwe pliku do zapisu\n");
		if (strcmp(argv[1], bufRead) == 0) {
			fifoOut = argv[1];
			fifoIn = argv[2];
		}
		else {
			fifoOut = argv[2];
			fifoIn = argv[1];
		}
		close(fildesTemp);
	}
	fildesIn = (int) open(fifoIn, O_RDWR);
	if (fildesIn == -1 || !checkIfFifo(fildesIn)) {
        perror("blad otwarcia FIFO: ");
        exit(EXIT_FAILURE);
	}
	fildesOut = (int) open(fifoOut, O_RDWR);
	if (fildesIn == -1 || !checkIfFifo(fildesOut)) {
        perror("blad otwarcia FIFO: ");
        exit(EXIT_FAILURE);
	}
	fildesCtlIn = (int) open(fifoCtlIn, O_RDWR);
	if (fildesCtlIn == -1 || !checkIfFifo(fildesCtlIn)) {
		perror("blad otwarcia FIFO: ");
		exit(EXIT_FAILURE);
	}
	fildesCtlOut = (int) open(fifoCtlOut, O_RDWR);
	if (fildesCtlOut == -1 || !checkIfFifo(fildesCtlOut)) {
		perror("blad otwarcia FIFO: ");
		exit(EXIT_FAILURE);
	}
    fildesShared = dup2(1, 100);

	printf("Fifo do zapisu: %s\n", fifoOut);
	printf("Fifo do oczytu: %s\n", fifoIn);
	printf("Fifo do zapisu kontrolek: %s\n", fifoCtlOut);
	printf("Fifo do odczytu kontrolek: %s\n", fifoCtlIn);
	globalCtlOut = fildesCtlOut;
	
	struct ArgStruct args;
	args.fildesIn = fildesIn;
	args.fildesOut = fildesOut;
	args.fildesShared = fildesShared;
	args.fildesCtlIn = fildesCtlIn;
	args.fildesCtlOut = fildesCtlOut;
	pthread_t threadWrite, threadRead;
	void* threadWriteRes,* threadReadRes;
	res1 = sem_init(&sem, 0, 0);
	if (res1 != 0) {
        printf("blad tworzenia semafora\n");
		exit(EXIT_FAILURE);
	}
	res2 = sem_init(&semError, 0, 1);
	if (res2 != 0) {
        printf("blad tworzenia semafora\n");
        sem_destroy(&sem);
		exit(EXIT_FAILURE);
	}

/*URUCHAMIANIE WATKOW*/

	res1 = pthread_create(&threadWrite, NULL, writingTh, (void* )&args);
	if (res1 != 0) {
        sem_wait(&sem);
		perror("blad tworzenia watka zapisu: ");
		sem_post(&sem);
		deleteSemaphores();
		exit(EXIT_FAILURE);
	}
	printf("Watek zapisu uruchomiony...\n");
	res2 = pthread_create(&threadRead, NULL, readingTh, (void*) &args);
	if (res2 != 0) {
        sem_wait(&sem);
		perror("blad tworzenia watka odczytu: ");
		sem_post(&sem);
        deleteSemaphores();
		exit(EXIT_FAILURE);
	}
	printf("Watek odczytu uruchomiony...\n");
	sem_post(&sem); //od teraz używamy fildesShared na output zamiast 1

/*KONCZENIE WATKOW*/

	res1 = pthread_join(threadWrite, &threadWriteRes);
	if (res1 != 0) {
		sem_wait(&sem);
		perror("blad zamkniecia watka zapisu: ");
        sem_post(&sem);
        pthread_cancel(threadRead);	
		exit(EXIT_FAILURE);
	}
    pthread_cancel(threadRead);     //jak koniec watka piszacego to watek czytajacy automatycznie jest konczony z buta
    printf("Zakonczono dzialanie watka sluchajacego\n");

	res2 = pthread_join(threadRead, &threadReadRes);
	if (res2 != 0) {
		sem_wait(&sem);
		perror("blad zamkniecia watka odczytu: ");
		sem_post(&sem);
		exit(EXIT_FAILURE);
	}
    printf("Zakonczono watki.\n");

	deleteSemaphores();
	close(fildesIn);
	close(fildesOut);
	close(fildesCtlIn);
	close(fildesCtlOut);
	if (master)
        close(fildesLOCK);
    else
        close(fildesTemp);
	deleteTempFiles();
	if (flagError == 2)
		printf("Program zakonczyl sie m.in. przez lenistwo we wpisywaniu komunikatow!\n");
	printf("Koniec programu. Nacisnij ENTER aby zakonczyc...\n");
    getchar();
	exit(0);

}
