#!/usr/bin/env python3
"""시연용 가상 회원 · 모집글 · 댓글을 만든다.

서버 API(/api/register, /api/login, /api/recruits ...)를 그대로 거치므로 비밀번호 해시와
표시 이름은 서버가 만든다. 다시 돌려도 이미 있는 학번·제목은 건너뛴다.

    python scripts/seed_demo.py                    # 기본 40명
    python scripts/seed_demo.py --count 60 --base http://localhost:8080

가상 계정은 모두 학번이 2099 로 시작하고(존재할 수 없는 입학 연도),
비밀번호는 demo1234! 이다. 지우려면 scripts/cleanup_demo.sql 을 실행한다.
"""

# 표준 라이브러리만 쓴다 (requests 같은 외부 패키지 없이 어느 PC 에서나 돌게)
import argparse
import http.cookiejar     # 로그인 세션 쿠키(sid)를 요청 사이에 보관
import json
import random
import sys
import urllib.error
import urllib.request     # HTTP 요청

PASSWORD = "demo1234!"       # 모든 가상 계정의 비밀번호
STUDENT_PREFIX = "2099"      # 가상 학번 접두어 (cleanup_demo.sql 이 이 접두어로 지운다)

# 가상 이름 재료: list("김이박...") 은 문자열을 한 글자씩 쪼갠 리스트가 된다
SURNAMES = list("김이박최정강조윤장임한오서신권황안송류홍전고문양손배백허유남심노하곽성차주우구민진나")
GIVEN = list("민서지윤도현하준유진수아예은시우재원승민서연지호다은태윤가은채원준혁은우소윤건우나연")

# 학과별 성향: (관심 분야 후보, 역할 후보, 해 본 활동, 찾는 팀)
# 키는 DB 의 학과 이름과 정확히 같아야 한다. 값의 키워드는 interest_tags 의 이름과 맞아야 태그로 들어간다.
PERSONAS = {
    "글로벌비즈니스어학부": (["글로벌", "마케팅", "글쓰기", "기획"], ["자료조사", "발표", "작가"],
                      ["교환학생 프로그램에 다녀왔고 통번역 봉사를 했습니다", "영어 발표 대회에 나간 적이 있습니다"],
                      ["해외 대상 홍보 아이디어 공모전", "국제개발협력 영상 챌린지"]),
    "경영학부": (["창업", "마케팅", "기획", "금융"], ["팀장", "기획자", "발표"],
             ["교내 창업동아리에서 사업계획서를 써 봤습니다", "소상공인 마케팅 컨설팅 봉사를 했습니다"],
             ["창업·아이디어 공모전", "전통시장 혁신 아이디어 공모전"]),
    "공공인재학부": (["공공정책", "사회문제", "글쓰기", "기획"], ["자료조사", "작가", "발표"],
               ["구청 청년 정책 제안단에서 활동했습니다", "인권 에세이 공모전에 입상한 적이 있습니다"],
               ["정책 제안·대국민 아이디어 공모전", "청렴·인권 콘텐츠 공모전"]),
    "아동청소년학과": (["교육", "사회문제", "글쓰기"], ["자료조사", "기획자", "작가"],
                ["지역아동센터 멘토링을 1년 했습니다", "청소년 진로 캠프 보조강사를 했습니다"],
                ["예방교육 우수사례 공모전", "청소년 안전 영상 공모전"]),
    "군사학과": (["공공정책", "스포츠", "사회문제"], ["팀장", "발표"],
             ["학군단 활동과 안보 토론회에 참여했습니다", "재난 대응 모의훈련 기획을 도왔습니다"],
             ["안전·안보 관련 공모전"]),
    "소프트웨어학과": (["개발", "AI", "데이터"], ["개발자", "기획자"],
                ["웹 서비스를 두 번 배포해 봤고 해커톤에 나간 적이 있습니다", "파이썬으로 데이터 분석 프로젝트를 했습니다"],
                ["AI·디지털 혁신 해커톤", "데이터 활용 아이디어 공모전"]),
    "금융정보공학과": (["금융", "데이터", "AI"], ["자료조사", "개발자", "기획자"],
                 ["금융 데이터 분석 스터디를 운영합니다", "핀테크 서비스 기획 공모전에 참가했습니다"],
                 ["금융 공모전", "데이터 분석 경진대회"]),
    "전자컴퓨터공학과": (["개발", "AI", "과학"], ["개발자", "자료조사"],
                  ["아두이노로 IoT 작품을 만들어 전시했습니다", "임베디드 동아리에서 드론을 만들었습니다"],
                  ["AI·디지털 혁신 공모전", "스마트시티 아이디어"]),
    "나노화학생명공학과": (["과학", "환경", "글쓰기"], ["자료조사", "작가"],
                   ["연구실 학부연구생으로 논문을 정리했습니다", "탄소중립 캠페인 서포터즈를 했습니다"],
                   ["과학기술 아이디어 공모전", "환경 캠페인 공모전"]),
    "물류시스템공학과": (["데이터", "기획", "도시건축"], ["기획자", "자료조사"],
                  ["물류센터 공정 개선 프로젝트를 했습니다", "수요 예측 데이터 분석을 해 봤습니다"],
                  ["에너지·수요관리 아이디어 공모전", "교통 데이터 공모전"]),
    "도시공학과": (["도시건축", "사회문제", "환경"], ["기획자", "자료조사", "디자이너"],
              ["도시재생 현장 조사에 참여했습니다", "마을 지도 만들기 프로젝트를 했습니다"],
              ["지역 혁신·도시 문제 해결 공모전", "층간소음·국토안전 공모전"]),
    "토목건축공학과": (["도시건축", "환경", "디자인"], ["디자이너", "자료조사"],
                ["건축 설계 스튜디오 작품을 전시했습니다", "시설물 안전 점검 실습을 했습니다"],
                ["국토안전 콘텐츠 공모전", "공간 디자인 공모전"]),
    "음악학부": (["음악", "공연"], ["작가", "발표"],
             ["오케스트라 정기연주회에 섰습니다", "작곡 전공으로 영상 배경음악을 만들어 봤습니다"],
             ["영상 공모전 배경음악 담당", "문화예술 콘텐츠 공모전"]),
    "실용음악학부": (["음악", "영상", "공연"], ["영상편집", "작가"],
               ["밴드에서 보컬을 맡고 있고 유튜브에 커버곡을 올립니다", "숏폼 음원을 직접 만들어 봤습니다"],
               ["유튜브·숏폼 영상 공모전", "홍보 영상 공모전"]),
    "무용예술학부": (["공연", "영상", "스포츠"], ["발표", "영상편집"],
               ["댄스 챌린지 영상을 기획해 조회수가 꽤 나왔습니다", "현대무용 공연에 출연했습니다"],
               ["챌린지 영상 공모전", "SNS 숏폼 공모전"]),
    "공연예술학부": (["공연", "영상", "글쓰기"], ["작가", "발표"],
               ["연극 동아리에서 대본을 쓰고 연출했습니다", "단편영화 배우로 참여했습니다"],
               ["스토리가 있는 영상 공모전", "시나리오 공모전"]),
    "디자인학부": (["디자인", "만화", "마케팅"], ["디자이너"],
              ["포스터·카드뉴스 공모전에서 입상했습니다", "브랜드 리디자인 프로젝트를 했습니다"],
              ["포스터·웹툰 공모전", "1컷 만화 공모전"]),
    "영화영상학과": (["영상", "글쓰기", "AI"], ["영상편집", "작가", "팀장"],
               ["단편영화 두 편을 연출했습니다", "AI 영상 생성 도구로 뮤직비디오를 만들어 봤습니다"],
               ["AI 영상 콘텐츠 공모전", "유튜브 영상 공모전"]),
    "광고홍보영상학과": (["마케팅", "영상", "디자인"], ["기획자", "영상편집", "발표"],
                  ["공익광고 캠페인을 기획해 상을 받았습니다", "SNS 계정 운영 대외활동을 했습니다"],
                  ["SNS 홍보 영상 공모전", "공익 캠페인 공모전"]),
    "헤어디자인학과": (["뷰티", "디자인", "사진"], ["디자이너"],
                ["헤어 화보 촬영 스타일링을 맡았습니다"], ["뷰티 콘텐츠 공모전", "사진 공모전"]),
    "메이크업디자인학과": (["뷰티", "공연", "사진"], ["디자이너"],
                   ["공연 분장팀으로 활동했습니다"], ["뷰티·공연 콘텐츠 공모전"]),
    "코스메틱&뷰티테라피학과": (["뷰티", "과학", "창업"], ["기획자", "자료조사"],
                        ["화장품 성분 연구 프로젝트를 했습니다"], ["뷰티 창업 아이디어 공모전"]),
    "미래융합학부1": (["창업", "기획", "사회문제"], ["팀장", "기획자"],
                 ["직장을 다니며 사내 혁신 아이디어 공모에 참여했습니다"], ["아이디어 공모전"]),
    "미래융합학부2": (["데이터", "기획", "창업"], ["기획자", "자료조사"],
                 ["재직 중에 업무 자동화 프로젝트를 했습니다"], ["데이터·디지털 아이디어 공모전"]),
    "아트앤테크놀로지학과": (["게임", "AI", "영상", "디자인"], ["개발자", "디자이너", "영상편집"],
                     ["유니티로 인터랙티브 미디어아트를 만들었습니다", "생성형 AI로 모션그래픽을 만들어 봤습니다"],
                     ["AI 영상·미디어아트 공모전", "메타버스 콘텐츠 공모전"]),
    "스포츠앤테크놀로지학과": (["스포츠", "데이터", "AI"], ["자료조사", "개발자"],
                      ["웨어러블로 운동 데이터를 모아 분석했습니다"], ["스포츠·헬스케어 아이디어 공모전"]),
    "자유전공학부": (["기획", "글쓰기", "사회문제", "사진"], ["자료조사", "작가", "기획자"],
               ["인문학 에세이 공모전에 글을 냈습니다", "교내 사진 동아리에서 활동합니다"],
               ["에세이·체험수기 공모전", "사진 공모전"]),
}
# 협업 성향 키워드 후보 (interest_tags 의 style 분류)
STYLES = ["꼼꼼함", "아이디어형", "실행력", "소통중시", "마감준수", "수상목표", "경험쌓기", "온라인협업", "대면모임"]

# (공모전 제목에 들어 있는 말, 모집글 제목, 찾는 키워드, 본문)
# 첫 번째 값으로 실제 게시물 제목을 찾아 모집글을 그 공모전에 연결한다 (못 찾으면 연결 없이).
RECRUITS = [
    ("문화다양성 AI 영상", "AI 영상 콘텐츠 공모전 같이 나갈 편집자·기획자 구해요",
     ["영상", "AI", "영상편집", "기획자"],
     "생성형 AI 로 3분 내외 영상을 만들 계획입니다. 촬영·편집 가능하신 분, 스토리 기획 좋아하시는 분 환영해요. 주 1회 온라인 회의 예정입니다."),
    ("서울교통공사 유튜브", "지하철 유튜브 영상 공모전 팀원 2명 모집",
     ["영상", "마케팅", "영상편집", "발표"],
     "지하철 이용 문화를 재밌게 알리는 숏폼을 찍으려고 합니다. 출연 가능하신 분이나 편집 잘하시는 분 찾습니다."),
    ("사회복무요원 SNS 영상", "SNS 숏폼 영상 공모전 - 연기·편집 가능하신 분",
     ["영상", "공연", "영상편집"],
     "20초~2분 숏폼이라 부담 없이 함께할 분 찾아요. 아이디어 회의부터 같이 하실 분이면 좋겠습니다."),
    ("전통시장 디지털 혁신", "전통시장 혁신 아이디어 공모전 기획·디자인 팀원",
     ["창업", "기획", "디자인", "기획자", "디자이너"],
     "시장 상인 인터뷰를 바탕으로 디지털 전환 아이디어를 내려고 합니다. 현장 조사 같이 다니실 분, 제안서 디자인 해 주실 분 구해요."),
    ("KRC AI 디지털 혁신", "AI 디지털 혁신 공모전 개발자 구합니다",
     ["개발", "AI", "데이터", "개발자"],
     "농어촌 문제를 AI 로 푸는 서비스 프로토타입을 만들 예정입니다. 백엔드나 데이터 분석 가능하신 분 찾습니다."),
    ("SB 스타트업 네트워킹", "지역문제 해결 해커톤 같이 나가실 분 (개발2, 기획1)",
     ["개발", "창업", "개발자", "기획자"],
     "성북구 지역 문제를 주제로 하는 해커톤입니다. 하루 반 동안 집중해서 MVP 만드실 분 구해요."),
    ("스마트폰 과의존", "과의존 예방교육 우수사례 공모전 자료조사·글쓰기 팀원",
     ["교육", "글쓰기", "자료조사", "작가"],
     "아동센터 멘토링 경험을 사례로 정리하려고 합니다. 자료 조사와 보고서 정리 꼼꼼하게 해 주실 분 환영합니다."),
    ("청소년 안전 AI 영상", "청소년 안전 AI 영상 공모전 함께해요",
     ["영상", "AI", "교육", "영상편집"],
     "안전 교육 내용을 AI 영상으로 풀어 보려 합니다. 시나리오 쓰실 분, 편집하실 분 모집합니다."),
    ("제주자치경찰 청렴 인권 포스터", "청렴·인권 포스터 공모전 디자이너 모십니다",
     ["디자인", "공공정책", "디자이너"],
     "메시지 기획은 제가 맡고, 포스터를 함께 완성할 디자이너를 찾고 있어요. 포트폴리오 있으면 좋아요."),
    ("과학기술 미래 상상 1컷 만화", "1컷 만화 공모전 그림 그리실 분",
     ["만화", "과학", "디자이너"],
     "과학기술 아이디어는 제가 정리했습니다. 그림 실력 있으신 분과 함께 출품하고 싶어요."),
    ("금융 공모전", "금융 공모전 카드뉴스 부문 팀원",
     ["금융", "디자인", "글쓰기", "디자이너"],
     "청년 금융 이해력을 주제로 카드뉴스를 만들 예정입니다. 금융 내용 정리하실 분과 디자인하실 분 찾습니다."),
    ("국토안전 AI 콘텐츠", "국토안전 AI 콘텐츠 공모전 영상팀",
     ["영상", "도시건축", "AI", "영상편집"],
     "시설물 안전을 알리는 짧은 AI 영상을 만듭니다. 토목·건축 지식 있으신 분 오시면 큰 도움이 됩니다."),
    ("층간소음 예방", "층간소음 예방 AI 영상 공모전 아이디어 회의부터 같이",
     ["영상", "도시건축", "사회문제"],
     "이웃 간 배려를 주제로 한 짧은 영상을 만들려고 해요. 아이디어 많은 분 환영합니다."),
    ("특허-산업 융합데이터", "데이터 활용 아이디어 공모전 분석 같이 하실 분",
     ["데이터", "개발", "기획", "자료조사"],
     "특허 데이터를 분석해서 산업 아이디어를 내는 공모전입니다. 파이썬이나 엑셀로 분석 가능하신 분 찾아요."),
]

# 모집글에 달 댓글 후보
COMMENTS = [
    "관심 있어요! 편집은 프리미어로 2년 정도 해 봤습니다.",
    "아직 자리 남았나요? 기획 쪽으로 참여하고 싶습니다.",
    "포트폴리오 보내드려도 될까요?",
    "온라인 회의 가능하면 참여하고 싶어요.",
    "비슷한 공모전 수상 경험 있습니다. 같이 해요!",
    "자료조사랑 발표 자신 있습니다. 연락 부탁드려요.",
    "일정이 맞으면 꼭 참여하고 싶습니다.",
    "개발 가능하고 해커톤 경험 있습니다.",
]


class Client:
    # 쿠키를 기억하는 작은 HTTP 클라이언트. 회원마다 하나씩 만들어 각자 로그인 상태를 유지한다.
    def __init__(self, base):
        self.base = base.rstrip("/")
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def call(self, method, path, body=None):
        # JSON 요청을 보내고 (상태 코드, 파싱한 JSON) 을 돌려준다. 4xx/5xx 도 예외 대신 값으로 돌려준다.
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"} if data else {})
        try:
            with self.opener.open(req, timeout=30) as r:
                return r.status, json.loads(r.read().decode("utf-8") or "null")
        except urllib.error.HTTPError as e:
            # urllib 은 200번대가 아니면 예외를 던지므로 여기서 본문(오류 JSON)을 읽어 돌려준다
            try:
                return e.code, json.loads(e.read().decode("utf-8"))
            except Exception:
                return e.code, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="http://localhost:8080")
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--seed", type=int, default=14)
    args = ap.parse_args()
    rnd = random.Random(args.seed)   # 시드를 고정하면 몇 번 돌려도 같은 가상 데이터가 나온다

    # 로그인하지 않은 클라이언트로 기초 정보(학과, 키워드, 게시물)를 읽는다
    anon = Client(args.base)
    st, depts = anon.call("GET", "/api/departments")
    st2, tags = anon.call("GET", "/api/tags")
    st3, posts = anon.call("GET", "/api/posts")
    if st != 200 or st2 != 200 or st3 != 200:
        sys.exit("서버에 연결할 수 없습니다. 서버를 먼저 띄우세요.")

    dept_list = [d for c in depts["colleges"] for d in c["departments"]]       # 단과대학 구조를 펼친 학과 목록
    tag_id = {t["name"]: t["id"] for g in tags["groups"] for t in g["tags"]}  # 키워드 이름 → id

    # ---------------------------------------------------------------- 회원
    users = []          # (student_no, dept_name)
    made = skipped = 0
    used_phones = set()
    for i in range(1, args.count + 1):
        # 처음에는 학과를 차례로 한 명씩 채우고(모든 학과에 최소 1명), 그 뒤로는 무작위
        dept = dept_list[(i - 1) % len(dept_list)] if i <= len(dept_list) else rnd.choice(dept_list)
        fields, roles, acts, wants = PERSONAS.get(dept["name"], (["기획"], ["자료조사"], ["다양한 활동"], ["공모전"]))
        chosen = rnd.sample(fields, k=min(len(fields), rnd.choice([2, 3])))
        if rnd.random() < 0.35:             # 다른 분야에도 관심이 있는 사람
            chosen.append(rnd.choice(["영상", "기획", "디자인", "데이터", "사회문제", "마케팅", "글쓰기"]))
        chosen += rnd.sample(roles, k=1)
        chosen += rnd.sample(STYLES, k=rnd.choice([1, 2]))
        chosen = list(dict.fromkeys(chosen))[:8]   # 순서를 지키며 중복 제거 + 서버 제한(8개)에 맞춤

        # 표시 이름이 "이름_전화가운데" 라 겹치지 않도록 전화번호를 중복 없이 뽑는다
        while True:
            phone = f"010-{rnd.randint(2000, 9899)}-{rnd.randint(1000, 9999)}"
            if phone not in used_phones:
                used_phones.add(phone)
                break
        name = rnd.choice(SURNAMES) + rnd.choice(GIVEN) + rnd.choice(GIVEN)
        grade = rnd.randint(1, 4)
        bio = (f"{dept['name']} {grade}학년입니다. {rnd.choice(acts)}. "
               f"{rnd.choice(wants)}에 같이 나갈 팀을 찾고 있어요.")
        student_no = f"{STUDENT_PREFIX}{i:06d}"   # 예: 2099000001 (6자리로 0 채움)

        st, out = anon.call("POST", "/api/register", {
            "student_no": student_no, "name": name, "phone": phone, "password": PASSWORD,
            "department_id": dept["id"], "interest_ids": [tag_id[t] for t in chosen if t in tag_id],
            "bio": bio,
        })
        if st == 201:
            made += 1
        elif st == 409:          # 이미 가입된 학번 (다시 돌린 경우)
            skipped += 1
        else:
            print("가입 실패", student_no, st, out)
            continue
        users.append(student_no)
    print(f"회원: 새로 {made}명, 이미 있음 {skipped}명")

    # ---------------------------------------------------------------- 모집글
    st, existing = anon.call("GET", "/api/recruits?status=all")
    have = {r["title"] for r in existing.get("recruits", [])} if st == 200 else set()
    sessions = {}   # 학번 → 로그인한 Client (같은 사람은 한 번만 로그인)

    def login(student_no):
        if student_no not in sessions:
            c = Client(args.base)
            st, _ = c.call("POST", "/api/login", {"student_no": student_no, "password": PASSWORD})
            sessions[student_no] = c if st == 200 else None
        return sessions[student_no]

    created = []
    for k, (needle, title, want, body) in enumerate(RECRUITS):
        if title in have:
            continue
        post = next((p for p in posts["posts"] if needle in p["title"]), None)   # 제목에 needle 이 든 첫 게시물
        author = users[(k * 3) % len(users)] if users else None                # 작성자를 골고루 나눠 맡긴다
        c = login(author) if author else None
        if not c:
            continue
        st, out = c.call("POST", "/api/recruits", {
            "title": title, "body": body, "post_id": post["id"] if post else 0,
            "need_people": rnd.choice([1, 2, 2, 3]),
            "tag_ids": [tag_id[t] for t in want if t in tag_id],
        })
        if st == 201:
            created.append((out["id"], author))
        else:
            print("모집글 실패", title, st, out)
    print(f"모집글: 새로 {len(created)}건")

    # ---------------------------------------------------------------- 댓글
    n_comments = 0
    for rid, author in created:
        others = [u for u in users if u != author]   # 작성자 본인은 댓글을 달지 않게
        for who in rnd.sample(others, k=min(len(others), rnd.choice([1, 2, 3]))):
            c = login(who)
            if c and c.call("POST", f"/api/recruits/{rid}/comments",
                            {"body": rnd.choice(COMMENTS)})[0] == 201:
                n_comments += 1
    # 마지막 두 건은 마감된 모집으로 둔다 (상태 표시 확인용)
    for rid, author in created[-2:]:
        login(author).call("PUT", f"/api/recruits/{rid}/status", {"status": "closed"})
    print(f"댓글: {n_comments}건")
    print(f"가상 계정 비밀번호: {PASSWORD}  (학번 {STUDENT_PREFIX}000001 ~)")


if __name__ == "__main__":
    main()
