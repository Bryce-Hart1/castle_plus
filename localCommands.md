for windows:
cd "C:\Users\swift\OneDrive\Desktop\swiftDev\castle+\castle_plus"
docker build -t castle .

take id and replace
docker ps                     # get the container name/ID
docker exec -it <container> nc -U /tmp/castle.sock