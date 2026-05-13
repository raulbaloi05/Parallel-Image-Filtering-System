
flow:

1.  imaginea este primita ca input (vector de bytes).

2.  ie converteste in structura Image folosind GraphicsMagick.

3.  imaginea este impartita in 4 zone egale.

4.  se creeaza 4 procese copil (fork).

5.  fiecare proces aplica un filtru pe zona sa.

6.  zonele procesate sunt salvate temporar.

7.  procesul parinte asteapta terminarea copiilor.

8.  imaginea finala este reconstruita din cele 4 parti.

9.  rezultatul este convertit inapoi in blob si returnat.


lista de filtre:
-   grayscale: black/white (tonuri de gri whatever stiti ce-i ala grayscale)
-   blur: da blur la imagine
-   sharpen: accentuare detalii
-   edge: detectare contur
-   negative: inversare culori




partea de paralelism:

fork() pentru a crea 4 procese copil care ruleaza in
paralel, fiecare procesand o parte a imaginii.



