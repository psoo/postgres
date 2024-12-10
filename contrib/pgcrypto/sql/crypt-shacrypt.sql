--
-- crypt() and gensalt: sha256crypt, sha512crypt
--

-- $5$ is sha256crypt
SELECT crypt('', '$5$Szzz0yzz');

SELECT crypt('foox', '$5$Szzz0yzz');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

-- generate a salt for sha256crypt, default rounds
UPDATE ctest SET salt = gen_salt('sha256crypt');
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

-- generate a salt for sha256crypt, rounds 9999
UPDATE ctest SET salt = gen_salt('sha256crypt', 9999);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

TRUNCATE ctest;

-- $6$ is sha512crypt
SELECT crypt('', '$6$Szzz0yzz');

SELECT crypt('foox', '$6$Szzz0yzz');

-- generate a salt for sha512crypt, default rounds
UPDATE ctest SET salt = gen_salt('sha512crypt');
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

-- generate a salt for sha512crypt, rounds 9999
UPDATE ctest SET salt = gen_salt('sha512crypt', 9999);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

-- cleanup
DROP TABLE ctest;